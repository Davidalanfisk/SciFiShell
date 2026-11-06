#include "MediaApp.h"

#include <cstring>
#include <filesystem>

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>

#include "stb_image.h"

namespace sf {

using ui::Rect;
using ui::State;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string dirOf(const std::string& p) {
    const size_t i = p.find_last_of("\\/");
    return i == std::string::npos ? std::string() : p.substr(0, i);
}

// Digit aware comparison so "Track 2" sorts before "Track 10".
int natCmp(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    auto dig = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
    while (i < a.size() && j < b.size()) {
        if (dig(a[i]) && dig(b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && dig(a[i2])) ++i2;
            while (j2 < b.size() && dig(b[j2])) ++j2;
            size_t zi = i, zj = j;
            while (zi + 1 < i2 && a[zi] == '0') ++zi;
            while (zj + 1 < j2 && b[zj] == '0') ++zj;
            const size_t li = i2 - zi, lj = j2 - zj;
            if (li != lj) return li < lj ? -1 : 1;
            const int c = a.compare(zi, li, b, zj, lj);
            if (c != 0) return c < 0 ? -1 : 1;
            i = i2;
            j = j2;
        } else {
            const int x = std::tolower(static_cast<unsigned char>(a[i])), y = std::tolower(static_cast<unsigned char>(b[j]));
            if (x != y) return x < y ? -1 : 1;
            ++i;
            ++j;
        }
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

std::string clock(double s) {
    if (s < 0 || s != s) s = 0;
    const int t = static_cast<int>(s);
    char b[24];
    if (t >= 3600) std::snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else std::snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}

void blendPx(uint8_t* p, Color8 c, float cover) {
    const float a = cover * c.a / 255.0f;
    if (a <= 0) return;
    const float pa = p[3] / 255.0f, oa = a + pa * (1 - a);
    p[0] = static_cast<uint8_t>((c.r * a + p[0] * pa * (1 - a)) / oa);
    p[1] = static_cast<uint8_t>((c.g * a + p[1] * pa * (1 - a)) / oa);
    p[2] = static_cast<uint8_t>((c.b * a + p[2] * pa * (1 - a)) / oa);
    p[3] = static_cast<uint8_t>(oa * 255);
}

// Fills a rounded rectangle with a colour computed per pixel (local coordinates).
template <class F>
void paintRound(Bitmap& bm, const Rect& r, float rad, F fn) {
    const int x0 = std::max(0, static_cast<int>(std::floor(r.x0))), x1 = std::min(bm.w, static_cast<int>(std::ceil(r.x1)));
    const int y0 = std::max(0, static_cast<int>(std::floor(r.y0))), y1 = std::min(bm.h, static_cast<int>(std::ceil(r.y1)));
    const float cx = (r.x0 + r.x1) * 0.5f, cy = (r.y0 + r.y1) * 0.5f, hx = r.w() * 0.5f - rad, hy = r.h() * 0.5f - rad;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const float dx = std::max(std::fabs(x + 0.5f - cx) - hx, 0.0f), dy = std::max(std::fabs(y + 0.5f - cy) - hy, 0.0f);
            const float cover = std::clamp(rad - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            if (cover <= 0) continue;
            blendPx(&bm.rgba[(static_cast<size_t>(y) * bm.w + x) * 4], fn(x + 0.5f - r.x0, y + 0.5f - r.y0), cover);
        }
}

Color8 sampleBilinear(const Bitmap& s, float u, float v) {
    const float fx = std::clamp(u * s.w - 0.5f, 0.0f, static_cast<float>(s.w - 1)), fy = std::clamp(v * s.h - 0.5f, 0.0f, static_cast<float>(s.h - 1));
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy), x1 = std::min(x0 + 1, s.w - 1), y1 = std::min(y0 + 1, s.h - 1);
    const float tx = fx - x0, ty = fy - y0;
    auto px = [&](int x, int y) { return &s.rgba[(static_cast<size_t>(y) * s.w + x) * 4]; };
    float o[4];
    for (int c = 0; c < 4; ++c) {
        const float a = px(x0, y0)[c] * (1 - tx) + px(x1, y0)[c] * tx, b = px(x0, y1)[c] * (1 - tx) + px(x1, y1)[c] * tx;
        o[c] = a * (1 - ty) + b * ty;
    }
    return Color8{static_cast<uint8_t>(o[0]), static_cast<uint8_t>(o[1]), static_cast<uint8_t>(o[2]), static_cast<uint8_t>(o[3])};
}

// Draws `src` centre-cropped to fill `r`.
void drawImage(Bitmap& bm, const Bitmap& src, const Rect& r, float rad) {
    if (src.w <= 0 || src.h <= 0 || r.w() < 1 || r.h() < 1) return;
    const float sa = static_cast<float>(src.w) / src.h, da = r.w() / r.h();
    float uw = 1, vh = 1;
    if (sa > da) uw = da / sa;
    else vh = sa / da;
    const float u0 = (1 - uw) * 0.5f, v0 = (1 - vh) * 0.5f;
    const float w = r.w(), h = r.h();
    paintRound(bm, r, rad, [&](float lx, float ly) { return sampleBilinear(src, u0 + lx / w * uw, v0 + ly / h * vh); });
}

// Draws a video frame letterboxed into rea. Frames the decoder already scaled to fit are copied row by row; anything else
// is resampled with fixed-point bilinear filtering.
// Letterboxes a frame of sw x sh into area (same sizing as blitFit) and cuts a transparent hole there, so the picture can be
// drawn on its own texture behind the panel. Returns the hole in bitmap pixels (w == 0: nothing).
void holeFit(Bitmap& bm, int sw, int sh, const Rect& area, int& ox, int& oy, int& ow, int& oh) {
    ow = oh = 0;
    if (sw <= 0 || sh <= 0 || area.w() < 2 || area.h() < 2) return;
    const float s = std::min(area.w() / sw, area.h() / sh);
    const int dw = std::max(1, static_cast<int>(sw * s + 0.5f)), dh = std::max(1, static_cast<int>(sh * s + 0.5f));
    const int dx = static_cast<int>((area.x0 + area.x1) * 0.5f - dw * 0.5f + 0.5f), dy = static_cast<int>((area.y0 + area.y1) * 0.5f - dh * 0.5f + 0.5f);
    const int x0 = std::max(0, dx), x1 = std::min(bm.w, dx + dw), y0 = std::max(0, dy), y1 = std::min(bm.h, dy + dh);
    if (x1 <= x0 || y1 <= y0) return;
    for (int y = y0; y < y1; ++y) std::memset(&bm.rgba[(static_cast<size_t>(y) * bm.w + x0) * 4], 0, static_cast<size_t>(x1 - x0) * 4);
    ox = x0;
    oy = y0;
    ow = x1 - x0;
    oh = y1 - y0;
}

void blitFit(Bitmap& bm, const Bitmap& src, const Rect& area) {
    if (src.w <= 0 || src.h <= 0 || area.w() < 2 || area.h() < 2) return;
    float s = std::min(area.w() / src.w, area.h() / src.h);
    if (std::fabs(s - 1.0f) < 0.02f) s = 1.0f;
    const int dw = std::max(1, static_cast<int>(src.w * s + 0.5f)), dh = std::max(1, static_cast<int>(src.h * s + 0.5f));
    const int dx = static_cast<int>((area.x0 + area.x1) * 0.5f - dw * 0.5f + 0.5f), dy = static_cast<int>((area.y0 + area.y1) * 0.5f - dh * 0.5f + 0.5f);
    const int xa = std::max(0, -dx), xb = std::min(dw, bm.w - dx), ya = std::max(0, -dy), yb = std::min(dh, bm.h - dy);
    if (xb <= xa || yb <= ya) return;
    if (dw == src.w && dh == src.h) {
        for (int y = ya; y < yb; ++y)
            std::memcpy(&bm.rgba[(static_cast<size_t>(dy + y) * bm.w + dx + xa) * 4], &src.rgba[(static_cast<size_t>(y) * src.w + xa) * 4], static_cast<size_t>(xb - xa) * 4);
        return;
    }
    const float fx = static_cast<float>(src.w) / dw, fy = static_cast<float>(src.h) / dh;
    std::vector<int> ix0(static_cast<size_t>(xb - xa)), ix1(ix0.size()), wx(ix0.size());
    for (int x = xa; x < xb; ++x) {
        const float sx = std::clamp((x + 0.5f) * fx - 0.5f, 0.0f, static_cast<float>(src.w - 1));
        const int i = static_cast<int>(sx), j = std::min(i + 1, src.w - 1);
        const size_t o = static_cast<size_t>(x - xa);
        ix0[o] = i * 4;
        ix1[o] = j * 4;
        wx[o] = static_cast<int>((sx - i) * 256);
    }
    for (int y = ya; y < yb; ++y) {
        const float sy = std::clamp((y + 0.5f) * fy - 0.5f, 0.0f, static_cast<float>(src.h - 1));
        const int j0 = static_cast<int>(sy), j1 = std::min(j0 + 1, src.h - 1), wy = static_cast<int>((sy - j0) * 256);
        const uint8_t* r0 = &src.rgba[static_cast<size_t>(j0) * src.w * 4];
        const uint8_t* r1 = &src.rgba[static_cast<size_t>(j1) * src.w * 4];
        uint8_t* d = &bm.rgba[(static_cast<size_t>(dy + y) * bm.w + dx + xa) * 4];
        for (int x = xa; x < xb; ++x, d += 4) {
            const size_t o = static_cast<size_t>(x - xa);
            const int a = ix0[o], b = ix1[o], w = wx[o];
            for (int c = 0; c < 3; ++c) {
                const int top = r0[a + c] * (256 - w) + r0[b + c] * w, bot = r1[a + c] * (256 - w) + r1[b + c] * w;
                d[c] = static_cast<uint8_t>((top * (256 - wy) + bot * wy) >> 16);
            }
            d[3] = 255;
        }
    }
}

Color8 hsv(float h, float s, float v) {
    h = h - std::floor(h);
    const float c = v * s, x = c * (1 - std::fabs(std::fmod(h * 6, 2.0f) - 1)), m = v - c;
    float r = 0, g = 0, b = 0;
    const int k = static_cast<int>(h * 6);
    switch (k % 6) {
        case 0: r = c; g = x; break;
        case 1: r = x; g = c; break;
        case 2: g = c; b = x; break;
        case 3: g = x; b = c; break;
        case 4: r = x; b = c; break;
        default: r = c; b = x; break;
    }
    auto q = [](float f) { return static_cast<uint8_t>(std::clamp(f, 0.0f, 1.0f) * 255 + 0.5f); };
    return Color8{q(r + m), q(g + m), q(b + m), 255};
}

// Stand-in for missing art: a dark gradient in a colour taken from the name, with its initial.
void drawPlaceholder(Bitmap& bm, const Rect& r, float rad, const std::string& name, const std::string& initialFrom) {
    const size_t hsh = std::hash<std::string>{}(name);
    const float hue = (hsh % 1000) / 1000.0f;
    const Color8 c0 = hsv(hue, 0.55f, 0.42f), c1 = hsv(hue + 0.08f, 0.6f, 0.14f);
    const float w = r.w(), h = r.h();
    paintRound(bm, r, rad, [&](float lx, float ly) { return ui::mix(c0, c1, std::clamp((lx / w + ly / h) * 0.5f, 0.0f, 1.0f)); });
    std::string ini;
    for (unsigned char ch : initialFrom)
        if (std::isalnum(ch)) { ini = std::string(1, static_cast<char>(std::toupper(ch))); break; }
    if (ini.empty()) ini = "#";
    const float px = std::min(w, h) * 0.5f;
    const float tw = measureText(ini, px, FontFace::UIBold);
    drawText(bm, r.x0 + (w - tw) * 0.5f, r.y0 + h * 0.5f + px * 0.35f, ini, px, FontFace::UIBold, Color8{255, 255, 255, 70});
}

void triangle(Bitmap& bm, float cx, float cy, float size, bool right, Color8 c) {
    const float h = size * 0.5f;
    for (float y = -h; y <= h; y += 1.0f) {
        const float w = (1 - std::fabs(y) / h) * size * 0.9f;
        if (right) ui::fillRect(bm, cx - size * 0.35f, cy + y, cx - size * 0.35f + w, cy + y + 1, c);
        else ui::fillRect(bm, cx + size * 0.35f - w, cy + y, cx + size * 0.35f, cy + y + 1, c);
    }
}

bool popUtf8(std::string& s) {
    if (s.empty()) return false;
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    s.erase(i);
    return true;
}

std::string mediaDir() {
    const char* la = std::getenv("LOCALAPPDATA");
    return std::string(la ? la : ".") + "\\SciFiShell\\media\\";
}

}  // namespace
namespace {

std::string durText(double s) {
    const int m = static_cast<int>(s / 60.0 + 0.5);
    if (m >= 60) return std::to_string(m / 60) + "H " + std::to_string(m % 60) + "M";
    return std::to_string(m) + " MIN";
}

std::string joinDot(const std::vector<std::string>& parts) {
    std::string o;
    for (const std::string& p : parts) {
        if (p.empty()) continue;
        if (!o.empty()) o += "  /  ";
        o += p;
    }
    return o;
}

}  // namespace

struct MediaApp::PCtx {
    Bitmap& L;
    ui::Rect view;
    float k, pad, gap;
    ui::Tokens t;
    std::function<void(const ui::Rect&, int, int)> hit;
    std::function<bool(int, int)> hot;
    std::function<void(const std::string&, const std::string&)> empty;
    bool onScreen(float y0, float y1) const { return y1 > view.y0 && y0 < view.y1; }
};


MediaApp::MediaApp(Renderer& renderer, MediaHost host) : PanelApp(renderer, 1600, 900, 2.2f), host_(std::move(host)) {
    title_ = "MEDIA";
    tagCache_ = db_.allTags();
    loadStations();
}

void MediaApp::loadStations() {
    stations_ = {
        {"Groove Salad", "https://ice1.somafm.com/groovesalad-128-mp3", "Ambient / downtempo"},
        {"Drone Zone", "https://ice1.somafm.com/dronezone-128-mp3", "Deep ambient"},
        {"Space Station Soma", "https://ice1.somafm.com/spacestation-128-mp3", "Electronic / space"},
        {"Deep Space One", "https://ice1.somafm.com/deepspaceone-128-mp3", "Deep space ambient"},
        {"Secret Agent", "https://ice1.somafm.com/secretagent-128-mp3", "Lounge / spy"},
        {"DEF CON Radio", "https://ice1.somafm.com/defcon-128-mp3", "Hacker beats"},
        {"Lush", "https://ice1.somafm.com/lush-128-mp3", "Mellow vocals"},
        {"Fluid", "https://ice1.somafm.com/fluid-128-mp3", "Instrumental hiphop"},
        {"Underground 80s", "https://ice1.somafm.com/u80s-128-mp3", "Synth pop"},
        {"Indie Pop Rocks", "https://ice1.somafm.com/indiepop-128-mp3", "Indie"},
        {"Beat Blender", "https://ice1.somafm.com/beatblender-128-mp3", "Downtempo / house"},
        {"Cliqhop IDM", "https://ice1.somafm.com/cliqhop-128-mp3", "Glitch / IDM"},
        {"Radio Paradise", "https://stream.radioparadise.com/mp3-128", "Eclectic"},
    };
    std::ifstream in(mediaDir() + "radio.txt");
    std::string ln;
    while (std::getline(in, ln)) {
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        if (ln.empty() || ln[0] == '#') continue;
        const size_t a = ln.find('|');
        if (a == std::string::npos) continue;
        const size_t b = ln.find('|', a + 1);
        Station s;
        s.name = ln.substr(0, a);
        s.url = b == std::string::npos ? ln.substr(a + 1) : ln.substr(a + 1, b - a - 1);
        s.genre = b == std::string::npos ? "Custom" : ln.substr(b + 1);
        if (!s.name.empty() && s.url.rfind("http", 0) == 0) stations_.push_back(s);
    }
}

void MediaApp::onOpen() {
    libCount_ = ~size_t(0);
    if (host_.count && host_.count() == 0 && host_.scanning && !host_.scanning() && host_.addDefaultFolders) host_.addDefaultFolders();
    layoutDirty_ = true;
}

void MediaApp::setTab(int t) {
    tab_ = std::clamp(t, 0, 4);
    theatre_ = false;
    manage_ = false;
    albumOpen_ = -1;
    showOpen_ = movieOpen_ = artistOpen_ = lyricsItem_ = -1;
    sortMode_ = filterMode_ = 0;
    scroll_ = scrollTarget_ = 0;
    invalidate();
}

void MediaApp::exitCinema() {
    if (!cinema_) return;
    cinema_ = false;
    if (cinemaFs_ && fullscreen()) toggleFullscreen();
    cinemaFs_ = false;
    invalidate();
}

void MediaApp::onResize(int, int) {
    if (cinema_ && !fullscreen()) {
        cinema_ = false;
        cinemaFs_ = false;
    }
    frame_.rgba.shrink_to_fit();
}

bool MediaApp::back() {
    if (cinema_) {
        exitCinema();
        return true;
    }
    if (searchFocus_) {
        searchFocus_ = false;
        invalidate();
        return true;
    }
    if (manage_) {
        manage_ = false;
        invalidate();
        return true;
    }
    if (theatre_) {
        theatre_ = false;
        invalidate();
        return true;
    }
    if (lyricsItem_ >= 0) {
        lyricsItem_ = -1;
        invalidate();
        return true;
    }
    if (showOpen_ >= 0 || movieOpen_ >= 0 || albumOpen_ >= 0 || artistOpen_ >= 0) {
        if (showOpen_ >= 0) showOpen_ = -1;
        else if (movieOpen_ >= 0) movieOpen_ = -1;
        else if (albumOpen_ >= 0) albumOpen_ = -1;
        else artistOpen_ = -1;
        scroll_ = scrollTarget_ = 0;
        invalidate();
        return true;
    }
    if (!q_.empty()) {
        q_.clear();
        invalidate();
        return true;
    }
    return false;
}

void MediaApp::reload() {
    const std::vector<MediaTrack> all = host_.tracks ? host_.tracks() : std::vector<MediaTrack>();
    items_.clear();
    vids_.clear();
    for (const MediaTrack& t : all) {
        if (t.video) {
            Vid v;
            v.t = t;
            v.dir = dirOf(t.path);
            auto vc = vcCache_.find(t.path);
            if (vc == vcCache_.end()) vc = vcCache_.emplace(t.path, classifyVideo(t.path)).first;
            v.vc = vc->second;
            vids_.push_back(std::move(v));
        } else {
            Item it;
            it.t = t;
            items_.push_back(std::move(it));
            if (!tagCache_.count(t.path) && tagAsked_.insert(t.path).second) {
                meta_.requestTags(t.path);
                ++tagsOutstanding_;
            }
        }
    }
    std::sort(vids_.begin(), vids_.end(), [](const Vid& a, const Vid& b) {
        const int d = natCmp(a.dir, b.dir);
        if (d != 0) return d < 0;
        return natCmp(a.t.title, b.t.title) < 0;
    });
    groupLibrary();
    libCount_ = host_.count ? host_.count() : 0;
    libScanning_ = host_.scanning ? host_.scanning() : false;
    albumsDirty_ = true;
    albumT_ = 99;
    reloadT_ = 0;
}

void MediaApp::rebuildAlbums() {
    std::string openKey;
    if (albumOpen_ >= 0 && albumOpen_ < static_cast<int>(albums_.size())) openKey = albums_[albumOpen_].key;
    albums_.clear();
    std::map<std::string, int> index;
    for (size_t i = 0; i < items_.size(); ++i) {
        Item& it = items_[i];
        auto tc = tagCache_.find(it.t.path);
        if (tc != tagCache_.end()) it.m = tc->second;
        it.title = it.m.title.empty() ? it.t.title : it.m.title;
        const std::string key = lower(it.m.album) + "|" + lower(dirOf(it.t.path));
        auto f = index.find(key);
        if (f == index.end()) {
            Album a;
            a.key = key;
            a.title = it.m.album.empty() ? it.t.folder : it.m.album;
            a.artist = it.m.albumArtist.empty() ? it.m.artist : it.m.albumArtist;
            a.year = it.m.year;
            f = index.emplace(key, static_cast<int>(albums_.size())).first;
            albums_.push_back(std::move(a));
        }
        Album& a = albums_[f->second];
        if (a.artist.empty() && it.m.artist.empty() && it.m.albumArtist.empty()) a.artist = std::filesystem::u8path(dirOf(it.t.path)).parent_path().filename().u8string();
        const std::string who = it.m.albumArtist.empty() ? it.m.artist : it.m.albumArtist;
        if (a.artist.empty()) a.artist = who;
        else if (!who.empty() && lower(who) != lower(a.artist)) a.artist = "Various Artists";
        if (a.year == 0) a.year = it.m.year;
        a.items.push_back(static_cast<int>(i));
    }
    for (Album& a : albums_)
        std::sort(a.items.begin(), a.items.end(), [&](int x, int y) {
            const Item &p = items_[x], &q = items_[y];
            const int tp = p.m.track > 0 ? p.m.track : 1 << 20, tq = q.m.track > 0 ? q.m.track : 1 << 20;
            if (tp != tq) return tp < tq;
            return natCmp(p.title, q.title) < 0;
        });
    std::sort(albums_.begin(), albums_.end(), [](const Album& a, const Album& b) {
        const int d = natCmp(a.artist, b.artist);
        if (d != 0) return d < 0;
        return natCmp(a.title, b.title) < 0;
    });
    std::string artistKeyOpen;
    if (artistOpen_ >= 0 && artistOpen_ < static_cast<int>(artists_.size())) artistKeyOpen = artists_[artistOpen_].key;
    artists_.clear();
    {
        std::map<std::string, int> ai;
        for (size_t i = 0; i < albums_.size(); ++i) {
            const std::string nm = albums_[i].artist.empty() ? "Unknown Artist" : albums_[i].artist;
            const std::string key = keyOf(nm);
            auto f = ai.find(key);
            if (f == ai.end()) {
                Artist a;
                a.name = nm;
                a.key = key;
                f = ai.emplace(key, static_cast<int>(artists_.size())).first;
                artists_.push_back(std::move(a));
            }
            artists_[f->second].albums.push_back(static_cast<int>(i));
            artists_[f->second].tracks += static_cast<int>(albums_[i].items.size());
        }
        std::sort(artists_.begin(), artists_.end(), [](const Artist& a, const Artist& b) { return natCmp(a.name, b.name) < 0; });
    }
    artistOpen_ = -1;
    for (size_t i = 0; i < artists_.size(); ++i)
        if (!artistKeyOpen.empty() && artists_[i].key == artistKeyOpen) artistOpen_ = static_cast<int>(i);
    albumOpen_ = -1;
    if (!openKey.empty())
        for (size_t i = 0; i < albums_.size(); ++i)
            if (albums_[i].key == openKey) albumOpen_ = static_cast<int>(i);
    nowPathSeen_.clear();
    albumsDirty_ = false;
    albumT_ = 0;
    if (!theatre_) invalidate();  // the library isn't on screen while a video fills the window
}

void MediaApp::pump() {
    {
        std::string done;
        int d = 0;
        while (d < 20 && scrape_.poll(done)) {
            ++d;
            if (done.rfind("img:", 0) == 0) {
                for (auto it = imgPath_.begin(); it != imgPath_.end();) it = it->second.empty() ? imgPath_.erase(it) : std::next(it);
            } else {
                infoCache_.erase(done);
                epCache_.erase(done);
            }
            if (!theatre_) invalidate();
        }
    }
    MetaResult r;
    int n = 0;
    std::vector<std::pair<std::string, TrackMeta>> fresh;
    while (n < 40 && meta_.poll(r)) {
        ++n;
        if (r.kind == MetaResult::Tags) {
            tagCache_[r.key] = r.meta;
            if (r.ok) fresh.emplace_back(r.key, r.meta);
            if (tagsOutstanding_ > 0) --tagsOutstanding_;
            albumsDirty_ = true;
        } else if (r.kind == MetaResult::Art) {
            if (r.ok) artCache_[r.key] = std::move(r.bmp);
            else artMissing_.insert(r.key);
            if (!theatre_) invalidate();
        } else {
            if (r.ok) thumbCache_[r.key] = std::move(r.bmp);
            else thumbMissing_.insert(r.key);
            if (!theatre_) invalidate();
        }
    }
    if (!fresh.empty()) db_.putTags(fresh);
}

bool MediaApp::albumMatches(const Album& a) const {
    if (q_.empty()) return true;
    const std::string q = lower(q_);
    return lower(a.title).find(q) != std::string::npos || lower(a.artist).find(q) != std::string::npos;
}

const Bitmap* MediaApp::artFor(const Album& a) {
    auto f = artCache_.find(a.key);
    if (f != artCache_.end()) return &f->second;
    if (artMissing_.count(a.key) && !a.artist.empty()) {  // no embedded or folder art: fall back to the scraped cover
        const Info* ai = info(MediaScrape::albumKey(a.artist, a.title));
        if (!ai) { scrape_.requestAlbum(a.artist, a.title); return nullptr; }
        return ai->found && !ai->img.empty() ? webBitmap(ai->img, 400, 400) : nullptr;
    }
    if (a.items.empty() || artMissing_.count(a.key) || artAsked_.count(a.key)) return nullptr;
    const Item& first = items_[a.items[0]];
    if (!tagCache_.count(first.t.path)) {  // wait for the tags: the album key may still change
        if (tagAsked_.insert(first.t.path + "#urgent").second) meta_.requestTags(first.t.path, true);
        return nullptr;
    }
    artAsked_.insert(a.key);
    meta_.requestArt(a.key, first.t.path);
    return nullptr;
}

const Bitmap* MediaApp::thumbFor(Vid& v) {
    auto f = thumbCache_.find(v.t.path);
    if (f != thumbCache_.end()) return &f->second;
    if (thumbMissing_.count(v.t.path) || thumbAsked_.count(v.t.path)) return nullptr;
    thumbAsked_.insert(v.t.path);
    meta_.requestThumb(v.t.path, v.t.path);
    return nullptr;
}

void MediaApp::playAlbum(int album, int from, bool shuffled) {
    if (album < 0 || album >= static_cast<int>(albums_.size()) || !host_.play) return;
    std::vector<std::string> paths;
    for (int i : albums_[album].items) paths.push_back(items_[i].t.path);
    if (paths.empty()) return;
    if (shuffled) {
        std::mt19937 rng(static_cast<unsigned>(SDL_GetTicks()));
        std::shuffle(paths.begin(), paths.end(), rng);
        from = 0;
    }
    host_.play(paths, std::clamp(from, 0, static_cast<int>(paths.size()) - 1));
}

int MediaApp::hitAt(float x, float y) const {
    for (size_t i = hits_.size(); i-- > 0;)
        if (hits_[i].r.contains(x, y)) return hits_[i].id * 100000 + hits_[i].arg;
    return -1;
}

void MediaApp::tick(double dt) {
    clockT_ += dt;
    reloadT_ += dt;
    albumT_ += dt;

    NowInfo n = host_.now ? host_.now() : NowInfo();
    const bool posChanged = static_cast<int>(n.pos) != static_cast<int>(now_.pos);
    const bool changed = n.active != now_.active || n.playing != now_.playing || n.title != now_.title || n.path != now_.path ||
                         n.loading != now_.loading || n.shuffle != now_.shuffle ||
                         n.repeat != now_.repeat || std::fabs(n.vol - now_.vol) > 0.001f || n.live != now_.live;
    now_ = n;
    if (changed) invalidate();
    else if (posChanged) {
        if (skValid_ && !isDirty()) seekDirty_ = true;
        else invalidate();
    }

    const bool scanning = host_.scanning ? host_.scanning() : false;
    const size_t cnt = host_.count ? host_.count() : 0;
    if ((cnt != libCount_ || scanning != libScanning_) && (!scanning || reloadT_ > 1.5)) reload();
    pump();
    recordProgress();
    if (albumsDirty_ && (albumT_ > 0.8 || tagsOutstanding_ == 0)) rebuildAlbums();

    // The spectrum is only analysed while the window is visible, and at the same ~25 Hz the window repaints at.
    specT_ += dt;
    if (!isOpen()) {
        specT_ = 0;
        if (bandsMoving_) {
            std::fill(std::begin(bands_), std::end(bands_), 0.0f);
            std::fill(std::begin(peaks_), std::end(peaks_), 0.0f);
            bandsMoving_ = false;
        }
    } else if (!cinema_ && specT_ >= 0.04 || (bandsMoving_ && !now_.playing && specT_ >= 0.02)) {
        const double sdt = std::min(specT_, 0.1);
        specT_ = 0;
        bool live = false;
        float raw[32] = {};
        if (now_.playing && host_.spectrum && host_.spectrum(raw, 32)) live = true;
        bool moving = false;
        for (int i = 0; i < 32; ++i) {
            const float target = live ? raw[i] : 0.0f;
            if (target > bands_[i]) bands_[i] += (target - bands_[i]) * static_cast<float>(std::min(1.0, sdt * 22));
            else bands_[i] = std::max(target, bands_[i] - static_cast<float>(sdt) * 1.4f);
            peaks_[i] = std::max(bands_[i], peaks_[i] - static_cast<float>(sdt) * 0.45f);
            if (bands_[i] > 0.01f || peaks_[i] > 0.01f) moving = true;
        }
        bandsMoving_ = moving;
    }
    const bool bandsMoving = bandsMoving_;

    const float d = scrollTarget_ - scroll_;
    bool scrolling = false;
    if (std::fabs(d) > 0.4f) {
        scroll_ += d * static_cast<float>(std::min(1.0, dt * 14));
        scrolling = true;
    } else if (d != 0) {
        scroll_ = scrollTarget_;
        invalidate();
    }
    if (now_.video) videoSeen_ = true;
    if (theatre_ && ((now_.active && !now_.video) || (!now_.active && videoSeen_))) {
        theatre_ = false;
        videoSeen_ = false;
        invalidate();
    }
    if (cinema_ && !theatre_) exitCinema();
    if (theatre_ && host_.videoTarget) {
        const float k = ph() / 900.0f;
        host_.videoTarget(cinema_ ? pw() : static_cast<int>(pw() - 72 * k), cinema_ ? ph() : static_cast<int>(ph() * 0.7f));
    }
    float fadeTarget = 1;
    if (cinema_) {
        idleT_ += dt;
        fadeTarget = (idleT_ < 2.5 || !now_.playing) ? 1.0f : 0.0f;
        const float step = static_cast<float>(dt) * (fadeTarget > uiFade_ ? 8.0f : 1.6f);
        const float nf = uiFade_ < fadeTarget ? std::min(fadeTarget, uiFade_ + step) : std::max(fadeTarget, uiFade_ - step);
        if (nf != uiFade_) {
            uiFade_ = nf;
            invalidate();
        }
    } else {
        uiFade_ = 1;
    }
    bool watching = false;
    if (theatre_) {
        watching = now_.playing;
        std::vector<uint8_t> px;
        int fw = 0, fh = 0;
        if (host_.videoFrame && host_.videoFrame(px, fw, fh) && fw > 0 && fh > 0 && px.size() >= static_cast<size_t>(fw) * fh * 4) {
            if (frame_.w != fw || frame_.h != fh) invalidate();  // the picture's rectangle changed: re-cut the hole
            frame_.w = fw;
            frame_.h = fh;
            frame_.rgba = std::move(px);
            vidFresh_ = true;
        }
    }
    const bool barsLive = bandsMoving && !cinema_;  // bars repaint on their own small rect, never the whole window
    specOnly_ = barsLive && !scrolling && !now_.loading;
    animating_ = barsLive || seekDirty_ || scrolling || now_.loading || (cinema_ && uiFade_ != fadeTarget);
}

bool MediaApp::onHover(float x, float y) {
    if (cinema_ && x >= 0 && (std::fabs(x - lastPx_) > 2 || std::fabs(y - lastPy_) > 2)) {
        lastPx_ = x;
        lastPy_ = y;
        idleT_ = 0;
    }
    const int h = x < 0 ? -1 : hitAt(x, y);
    if (h == hover_) return false;
    hover_ = h;
    return true;
}

void MediaApp::onWheel(float, float, float notches) {
    const float k = ph() / 900.0f;
    scrollTarget_ = std::clamp(scrollTarget_ - notches * 120.0f * k, 0.0f, scrollMax_);
}

void MediaApp::onKey(EditKey k) {
    if (!searchFocus_) {
        if ((k == EditKey::Up || k == EditKey::Down) && host_.now && host_.setVolume) {
            const NowInfo now = host_.now();
            host_.setVolume(std::clamp(now.vol + (k == EditKey::Up ? 0.1f : -0.1f), 0.0f, 1.0f));
            idleT_ = 0;
        } else if (k == EditKey::MediaPlayPause && host_.toggle) {
            host_.toggle();
            idleT_ = 0;
        }
        return;
    }
    if (k == EditKey::Backspace) {
        if (popUtf8(q_)) scroll_ = scrollTarget_ = 0;
    } else if (k == EditKey::Enter) {
        searchFocus_ = false;
    } else if (k == EditKey::Paste) {
        if (char* t = SDL_GetClipboardText()) {
            for (const char* c = t; *c; ++c)
                if (static_cast<unsigned char>(*c) >= 0x20) q_ += *c;
            SDL_free(t);
            scroll_ = scrollTarget_ = 0;
        }
    }
}

void MediaApp::onText(const std::string& s) {
    if (!searchFocus_) return;
    q_ += s;
    scroll_ = scrollTarget_ = 0;
}

void MediaApp::onCtrl(const std::string& s) {
    if (s == "esc") back();
}

void MediaApp::onClick(float x, float y, bool) {
    const int h = hitAt(x, y);
    if (h < 0) {
        searchFocus_ = false;
        return;
    }
    click(h / 100000, h % 100000, x, y);
}

void MediaApp::click(int id, int arg, float x, float) {
    if (id != 4) searchFocus_ = false;
    auto frac = [&](int hid) {
        for (const Hit& h : hits_)
            if (h.id == hid) return std::clamp((x - h.r.x0) / std::max(1.0f, h.r.w()), 0.0f, 1.0f);
        return 0.0f;
    };
    switch (id) {
        case 1: case 2: case 3: setTab(id - 1); break;
        case 5: case 6: setTab(id - 2); break;
        case 7: musicByAlbum_ = !musicByAlbum_; scroll_ = scrollTarget_ = 0; break;
        case 8: sortMode_ = (sortMode_ + 1) % 4; scroll_ = scrollTarget_ = 0; break;
        case 9: filterMode_ = (filterMode_ + 1) % 4; scroll_ = scrollTarget_ = 0; break;
        case 70: showOpen_ = arg; scroll_ = scrollTarget_ = 0; seasonSel_ = -1; break;
        case 71: showOpen_ = -1; scroll_ = scrollTarget_ = 0; break;
        case 72: seasonSel_ = arg; scroll_ = scrollTarget_ = 0; break;
        case 73: case 74: {  // an episode, or the show's CONTINUE / PLAY button
            if (showOpen_ < 0 || showOpen_ >= static_cast<int>(shows_.size())) break;
            const Show& s = shows_[showOpen_];
            int start = id == 73 ? arg : resumeIndex(s);
            std::vector<int> list = s.eps;
            const auto f = std::find(list.begin(), list.end(), start);
            playVids(list, f == list.end() ? 0 : static_cast<int>(f - list.begin()));
            break;
        }
        case 75: {
            const std::string pth = vids_[arg].t.path;
            const Progress p = prog(pth);
            db_.setProgress(pth, 0, p.dur, !p.watched);
            progress_[pth] = db_.progress(pth);
            break;
        }
        case 80: movieOpen_ = arg; scroll_ = scrollTarget_ = 0; break;
        case 81: movieOpen_ = -1; scroll_ = scrollTarget_ = 0; break;
        case 82: if (arg >= 0 && arg < static_cast<int>(vids_.size())) playVids({arg}, 0); break;
        case 90: artistOpen_ = arg; scroll_ = scrollTarget_ = 0; break;
        case 91: artistOpen_ = -1; scroll_ = scrollTarget_ = 0; break;
        case 92:
            if (artistOpen_ >= 0 && artistOpen_ < static_cast<int>(artists_.size()) && host_.play) {
                std::vector<std::string> paths;
                for (int ai : artists_[artistOpen_].albums)
                    for (int i : albums_[ai].items) paths.push_back(items_[i].t.path);
                std::mt19937 rng(static_cast<unsigned>(SDL_GetTicks()));
                std::shuffle(paths.begin(), paths.end(), rng);
                if (!paths.empty()) host_.play(paths, 0);
            }
            break;
        case 93: {
            lyricsItem_ = arg;
            if (arg >= 0 && arg < static_cast<int>(items_.size()) && albumOpen_ >= 0) {
                const Item& it = items_[arg];
                const std::string ar = it.m.artist.empty() ? albums_[albumOpen_].artist : it.m.artist;
                scrape_.requestLyrics(ar, it.title, it.m.seconds);
            }
            break;
        }
        case 94: lyricsItem_ = -1; break;
        case 4: searchFocus_ = true; break;
        case 10: if (host_.toggle) host_.toggle(); break;
        case 11: if (host_.prev) host_.prev(); break;
        case 12: if (host_.next) host_.next(); break;
        case 13: if (host_.toggleShuffle) host_.toggleShuffle(); break;
        case 14: if (host_.cycleRepeat) host_.cycleRepeat(); break;
        case 15: if (host_.seek && now_.dur > 0) host_.seek(frac(15) * now_.dur); break;
        case 16: if (host_.setVolume) host_.setVolume(frac(16)); break;
        case 17: vizMode_ = (vizMode_ + 1) % 4; break;
        case 20: albumOpen_ = arg; scroll_ = scrollTarget_ = 0; break;
        case 21: playAlbum(arg, 0, false); break;
        case 22: albumOpen_ = -1; lyricsItem_ = -1; scroll_ = scrollTarget_ = 0; break;
        case 23: playAlbum(albumOpen_, 0, false); break;
        case 24: playAlbum(albumOpen_, 0, true); break;
        case 25: playAlbum(albumOpen_, arg, false); break;
        case 30: case 31: {  // a video, or "play all" of its folder
            if (arg < 0 || arg >= static_cast<int>(vids_.size()) || !host_.play) break;
            std::vector<std::string> paths;
            int idx = 0;
            for (size_t i = 0; i < vids_.size(); ++i)
                if (vids_[i].dir == vids_[arg].dir) {
                    if (static_cast<int>(i) == arg && id == 30) idx = static_cast<int>(paths.size());
                    paths.push_back(vids_[i].t.path);
                }
            host_.play(paths, idx);
            theatre_ = true;
            videoSeen_ = false;
            frame_ = Bitmap();
            break;
        }
        case 60: theatre_ = false; exitCinema(); break;
        case 62: exitCinema(); break;
        case 63:
            if (!cinema_) {
                cinema_ = true;
                cinemaFs_ = !fullscreen();
                idleT_ = 0;
                uiFade_ = 1;
                if (!fullscreen()) toggleFullscreen();
            }
            break;
        case 52: if (host_.pickFolders) host_.pickFolders(); break;
        case 61:
            if (now_.video) theatre_ = true;
            else if (nowAlbum_ >= 0) { tab_ = 0; theatre_ = false; albumOpen_ = nowAlbum_; scroll_ = scrollTarget_ = 0; }
            break;
        case 40:
            if (arg >= 0 && arg < static_cast<int>(stations_.size()) && host_.playStream) host_.playStream(stations_[arg].name, stations_[arg].url);
            break;
        case 50: if (host_.addDefaultFolders) host_.addDefaultFolders(); break;
        case 51: manage_ = !manage_; scroll_ = scrollTarget_ = 0; break;
        case 53: {
            const std::vector<std::string> fl = host_.folders ? host_.folders() : std::vector<std::string>{};
            if (arg >= 0 && arg < static_cast<int>(fl.size()) && host_.removeFolder) host_.removeFolder(fl[static_cast<size_t>(arg)]);
            break;
        }
        default: break;
    }
    invalidate();
}

// True full screen: just the picture; the title, transport and seek bar fade in on pointer movement and fade out again.
void MediaApp::paintCinema(Bitmap& bm) {
    using namespace ui;
    const Tokens t = tokens();
    const float k = ph() / 900.0f, a = uiFade_;
    auto hot = [&](int id) { return hover_ == id * 100000; };
    auto A = [&](Color8 c, float f = 1.0f) { return withAlpha(c, static_cast<int>(c.a * std::clamp(a * f, 0.0f, 1.0f))); };
    auto addHit = [&](const Rect& r, int id) { hits_.push_back({r, id, 0}); };
    const Rect whole{0, 0, static_cast<float>(bm.w), static_cast<float>(bm.h)};
    fillRect(bm, 0, 0, whole.x1, whole.y1, Color8{0, 0, 0, 255});
    if (frame_.w > 0 && frame_.h > 0) {
        holeFit(bm, frame_.w, frame_.h, whole, vx_, vy_, vw_, vh_);
    } else {
        const std::string s = now_.loading ? "OPENING" : "LOADING VIDEO";
        label(bm, whole.x1 * 0.5f - measureText(s, 30 * k, FontFace::UIBold) * 0.5f, whole.y1 * 0.5f, s, 30 * k, FontFace::UIBold, t.dim);
    }
    addHit(whole, 10);
    if (a < 0.02f) return;

    // scrims keep the text readable over bright pictures
    const int topH = static_cast<int>(150 * k), botH = static_cast<int>(260 * k);
    for (int y = 0; y < topH; ++y) {
        const float f = 1.0f - static_cast<float>(y) / topH;
        fillRect(bm, 0, static_cast<float>(y), whole.x1, static_cast<float>(y + 1), Color8{0, 0, 0, static_cast<uint8_t>(165 * f * f * a)});
    }
    for (int y = 0; y < botH; ++y) {
        const float f = static_cast<float>(y) / botH;
        fillRect(bm, 0, whole.y1 - botH + y, whole.x1, whole.y1 - botH + y + 1, Color8{0, 0, 0, static_cast<uint8_t>(190 * f * f * a)});
    }
    const float m = 60 * k;
    label(bm, m, 70 * k, ellipsize(now_.title, 36 * k, FontFace::UIBold, whole.x1 - m * 2 - 360 * k), 36 * k, FontFace::UIBold, A(t.text));
    if (a > 0.3f) {
        const Rect ex{whole.x1 - m - 300 * k, 34 * k, whole.x1 - m, 34 * k + 50 * k};
        button(bm, ex, "EXIT FULLSCREEN", hot(62) ? State::Hover : State::Normal, false, 20 * k);
        addHit(ex, 62);
    }

    const float by = whole.y1 - 96 * k, cxm = whole.x1 * 0.5f, ox = 120 * k;
    const Color8 dark{8, 13, 20, 255};
    if (a > 0.3f) {
        const float pr = 34 * k;
        dot(bm, cxm, by, pr, hot(10) ? t.accent : withAlpha(t.accent, 215));
        if (now_.playing) {
            fillRect(bm, cxm - 11 * k, by - 13 * k, cxm - 3 * k, by + 13 * k, dark);
            fillRect(bm, cxm + 3 * k, by - 13 * k, cxm + 11 * k, by + 13 * k, dark);
        } else {
            triangle(bm, cxm, by, 28 * k, true, dark);
        }
        addHit(Rect{cxm - pr, by - pr, cxm + pr, by + pr}, 10);
        const Color8 pc = hot(11) ? t.accent : t.text, nc = hot(12) ? t.accent : t.text;
        triangle(bm, cxm - ox, by, 24 * k, false, pc);
        fillRect(bm, cxm - ox - 15 * k, by - 12 * k, cxm - ox - 11 * k, by + 12 * k, pc);
        triangle(bm, cxm + ox, by, 24 * k, true, nc);
        fillRect(bm, cxm + ox + 11 * k, by - 12 * k, cxm + ox + 15 * k, by + 12 * k, nc);
        addHit(Rect{cxm - ox - 28 * k, by - 28 * k, cxm - ox + 28 * k, by + 28 * k}, 11);
        addHit(Rect{cxm + ox - 28 * k, by - 28 * k, cxm + ox + 28 * k, by + 28 * k}, 12);
    } else {
        dot(bm, cxm, by, 34 * k, A(t.accent, 0.8f));
    }

    // seek bar with time stamps
    {
        const float sy = whole.y1 - 188 * k, tw = 110 * k;
        const Rect sk{m + tw, sy, whole.x1 - m - tw, sy + 8 * k};
        const float frac = now_.dur > 0 ? std::clamp(static_cast<float>(now_.pos / now_.dur), 0.0f, 1.0f) : 0.0f;
        fillRound(bm, sk.x0, sk.y0, sk.x1, sk.y1, 4 * k, A(t.text, 0.22f));
        if (frac > 0) fillRound(bm, sk.x0, sk.y0, sk.x0 + sk.w() * frac, sk.y1, 4 * k, A(t.accent));
        dot(bm, sk.x0 + sk.w() * frac, (sk.y0 + sk.y1) * 0.5f, (hot(15) ? 11 : 8) * k, A(t.text));
        label(bm, m, sy + 14 * k, clock(now_.pos), 22 * k, FontFace::UI, A(t.text, 0.85f));
        labelRight(bm, whole.x1 - m, sy + 14 * k, now_.dur > 0 ? clock(now_.dur) : "--:--", 22 * k, FontFace::UI, A(t.text, 0.85f));
        if (a > 0.3f && now_.dur > 0) addHit(Rect{sk.x0, sk.y0 - 22 * k, sk.x1, sk.y1 + 22 * k}, 15);
    }
    // volume
    {
        const Rect vr{whole.x1 - m - 280 * k, by - 6 * k, whole.x1 - m, by + 6 * k};
        label(bm, vr.x0 - 78 * k, by + 8 * k, "VOL", 20 * k, FontFace::UIBold, A(t.dim));
        const float v = std::clamp(now_.vol, 0.0f, 1.0f);
        fillRound(bm, vr.x0, vr.y0, vr.x1, vr.y1, 6 * k, A(t.text, 0.22f));
        fillRound(bm, vr.x0, vr.y0, vr.x0 + vr.w() * v, vr.y1, 6 * k, A(t.accent));
        dot(bm, vr.x0 + vr.w() * v, by, 9 * k, A(t.text));
        if (a > 0.3f) addHit(Rect{vr.x0, by - 22 * k, vr.x1, by + 22 * k}, 16);
    }
}

void MediaApp::snapshotBackdrop(const Bitmap& bm) {
    const int x0 = std::max(0, static_cast<int>(spRect_.x0)), x1 = std::min(bm.w, static_cast<int>(spRect_.x1) + 1);
    const int y0 = std::max(0, static_cast<int>(spRect_.y0)), y1 = std::min(bm.h, static_cast<int>(spRect_.y1) + 1);
    spValid_ = false;
    if (x1 <= x0 || y1 <= y0) return;
    spPx_ = {x0, y0, x1 - x0, y1 - y0};
    backdrop_.resize(static_cast<size_t>(spPx_.w) * spPx_.h * 4);
    for (int y = 0; y < spPx_.h; ++y)
        std::memcpy(&backdrop_[static_cast<size_t>(y) * spPx_.w * 4], &bm.rgba[(static_cast<size_t>(y0 + y) * bm.w + x0) * 4],
                    static_cast<size_t>(spPx_.w) * 4);
    spW_ = bm.w;
    spH_ = bm.h;
    spValid_ = true;
}

void MediaApp::drawBars(Bitmap& bm, const ui::Rect& sp) {
    using namespace ui;
    const float k = spK_;
    const int nb = 32;
    const float bwid = sp.w() / nb;
    if (vizMode_ == 1) {  // mirrored: bars grow from the centre line, with the peaks as thin caps
        const float mid = (sp.y0 + sp.y1) * 0.5f;
        for (int i = 0; i < nb; ++i) {
            const float v = std::clamp(bands_[i], 0.0f, 1.0f), pk = std::clamp(peaks_[i], 0.0f, 1.0f);
            const float h = std::max(1.5f * k, v * sp.h() * 0.5f);
            const Color8 col = mix(spA_, spB_, static_cast<float>(i) / nb);
            const float bx = sp.x0 + i * bwid;
            fillRect(bm, bx + 1.5f * k, mid - h, bx + bwid - 1.5f * k, mid + h, withAlpha(col, v > 0.02f ? 225 : 70));
            if (pk > 0.04f) {
                fillRect(bm, bx + 1.5f * k, mid - pk * sp.h() * 0.5f - 2 * k, bx + bwid - 1.5f * k, mid - pk * sp.h() * 0.5f, spT_);
                fillRect(bm, bx + 1.5f * k, mid + pk * sp.h() * 0.5f, bx + bwid - 1.5f * k, mid + pk * sp.h() * 0.5f + 2 * k, spT_);
            }
        }
        return;
    }
    if (vizMode_ == 2) {  // radial burst: spokes around a centre, the bass at the top going clockwise
        const float cx = (sp.x0 + sp.x1) * 0.5f, cy = (sp.y0 + sp.y1) * 0.5f;
        const float r0 = std::min(sp.w(), sp.h()) * 0.16f, rmax = std::min(sp.w(), sp.h()) * 0.5f;
        for (int i = 0; i < 64; ++i) {
            const int b = i < 32 ? i : 63 - i;  // mirror so the left and right halves match
            const float v = std::clamp(bands_[b], 0.0f, 1.0f);
            const float a = static_cast<float>(i) / 64 * 6.2831853f - 1.5707963f;
            const float r1 = r0 + std::max(1.0f * k, v * (rmax - r0));
            const Color8 col = mix(spA_, spB_, static_cast<float>(b) / nb);
            for (float r = r0; r < r1; r += 1.0f * k)
                fillRect(bm, cx + std::cos(a) * r - 1.0f * k, cy + std::sin(a) * r - 1.0f * k, cx + std::cos(a) * r + 1.0f * k, cy + std::sin(a) * r + 1.0f * k, withAlpha(col, 230));
        }
        return;
    }
    if (vizMode_ == 3) {  // dot matrix: a stack of small squares per band
        const int rows = 10;
        const float ch = sp.h() / rows;
        for (int i = 0; i < nb; ++i) {
            const int lit = static_cast<int>(std::clamp(bands_[i], 0.0f, 1.0f) * rows + 0.5f);
            const Color8 col = mix(spA_, spB_, static_cast<float>(i) / nb);
            const float bx = sp.x0 + i * bwid;
            for (int r = 0; r < rows; ++r)
                fillRect(bm, bx + 2 * k, sp.y1 - (r + 1) * ch + 1.5f * k, bx + bwid - 2 * k, sp.y1 - r * ch - 1.5f * k, withAlpha(col, r < lit ? 235 : 40));
        }
        return;
    }
    for (int i = 0; i < nb; ++i) {
        const float v = std::clamp(bands_[i], 0.0f, 1.0f), pk = std::clamp(peaks_[i], 0.0f, 1.0f);
        const float h = std::max(2.0f * k, v * sp.h());
        const Color8 col = mix(spA_, spB_, static_cast<float>(i) / nb);
        const float bx = sp.x0 + i * bwid;
        fillRect(bm, bx + 1.5f * k, sp.y1 - h, bx + bwid - 1.5f * k, sp.y1, withAlpha(col, v > 0.02f ? 235 : 70));
        if (pk > 0.04f) fillRect(bm, bx + 1.5f * k, sp.y1 - pk * sp.h() - 3 * k, bx + bwid - 1.5f * k, sp.y1 - pk * sp.h(), spT_);
    }
}

const Bitmap* MediaApp::videoLayer(int& x, int& y, int& w, int& h, bool& fresh) {
    if (!theatre_ || vw_ <= 0 || frame_.w <= 0 || frame_.h <= 0) return nullptr;
    x = vx_;
    y = vy_;
    w = vw_;
    h = vh_;
    fresh = vidFresh_;
    vidFresh_ = false;
    return &frame_;
}

void MediaApp::drawSeek(Bitmap& bm) {
    using namespace ui;
    const float k = skK_, sy = skY_;
    const Rect sk{skX0_ + 64 * k, sy, skX1_ - 64 * k, sy + 10 * k};
    const float frac = skSeekable_ ? std::clamp(static_cast<float>(now_.pos / now_.dur), 0.0f, 1.0f) : 0.0f;
    bar(bm, sk, now_.live ? 1.0f : frac, now_.live ? withAlpha(skAccent_, 60) : skAccent_);
    if (skSeekable_) dot(bm, sk.x0 + sk.w() * frac, (sk.y0 + sk.y1) * 0.5f, (hover_ == 15 * 100000 ? 9 : 7) * k, skText_);
    if (now_.live) {
        const std::string s = "LIVE";
        label(bm, (sk.x0 + sk.x1) * 0.5f - measureText(s, 17 * k, FontFace::UIBold) * 0.5f, sy - 8 * k, s, 17 * k, FontFace::UIBold, skAccent_);
    } else if (now_.active) {
        label(bm, skX0_, sy + 11 * k, clock(now_.pos), 18 * k, FontFace::UI, skDim_);
        labelRight(bm, skX1_, sy + 11 * k, now_.dur > 0 ? clock(now_.dur) : "--:--", 18 * k, FontFace::UI, skDim_);
    }
}

bool MediaApp::repaintPartial(Bitmap& bm, int& x, int& y, int& w, int& h) {
    const bool doBars = specOnly_ && spValid_ && bm.w == spW_ && bm.h == spH_;
    const bool doSeek = seekDirty_ && skValid_ && bm.w == skW_ && bm.h == skH_;
    if (!doBars && !doSeek) return false;
    auto restore = [&](const PxRect& r, const std::vector<uint8_t>& src) {
        for (int i = 0; i < r.h; ++i)
            std::memcpy(&bm.rgba[(static_cast<size_t>(r.y + i) * bm.w + r.x) * 4], &src[static_cast<size_t>(i) * r.w * 4], static_cast<size_t>(r.w) * 4);
    };
    int x0 = bm.w, y0 = bm.h, x1 = 0, y1 = 0;
    auto grow = [&](const PxRect& r) {
        x0 = std::min(x0, r.x);
        y0 = std::min(y0, r.y);
        x1 = std::max(x1, r.x + r.w);
        y1 = std::max(y1, r.y + r.h);
    };
    if (doBars) {
        restore(spPx_, backdrop_);
        drawBars(bm, ui::Rect{spRect_.x0, spRect_.y0 + 4 * spK_, spRect_.x1, spRect_.y1});
        grow(spPx_);
    }
    if (doSeek) {
        restore(skPx_, skBack_);
        drawSeek(bm);
        grow(skPx_);
        seekDirty_ = false;
    }
    x = x0;
    y = y0;
    w = x1 - x0;
    h = y1 - y0;
    return true;
}

void MediaApp::paint(Bitmap& bm) {
    using namespace ui;
    spValid_ = false;
    skValid_ = false;
    seekDirty_ = false;
    vw_ = vh_ = 0;
    const Tokens t = tokens();
    const float k = ph() / 900.0f;
    hits_.clear();
    if (cinema_ && theatre_ && fullscreen()) {
        paintCinema(bm);
        return;
    }
    const int prevHover = hover_;
    auto hot = [&](int id, int arg) { return hover_ == id * 100000 + arg; };
    auto addHit = [&](const Rect& r, int id, int arg = 0, const Rect* clip = nullptr) {
        Rect h = r;
        if (clip) {
            h.x0 = std::max(h.x0, clip->x0);
            h.x1 = std::min(h.x1, clip->x1);
            h.y0 = std::max(h.y0, clip->y0);
            h.y1 = std::min(h.y1, clip->y1);
        }
        if (h.x1 > h.x0 && h.y1 > h.y0) hits_.push_back({h, id, arg});
    };

    // header text
    size_t nAlb = albums_.size();
    std::string sub;
    if (tab_ == 0) sub = std::to_string(artists_.size()) + " ARTISTS  /  " + std::to_string(nAlb) + " ALBUMS  /  " + std::to_string(items_.size()) + " TRACKS";
    else if (tab_ == 3) sub = std::to_string(shows_.size()) + " SHOWS";
    else if (tab_ == 4) sub = std::to_string(movies_.size()) + " MOVIES";
    else if (tab_ == 1) sub = std::to_string(vids_.size()) + " VIDEOS";
    else sub = std::to_string(stations_.size()) + " STATIONS";
    if (libScanning_) sub = "SCANNING  " + std::to_string(host_.scanned ? host_.scanned() : 0) + " FILES  /  " + sub;
    const Rect c = panel(bm, "MEDIA", sub);

    // toolbar
    const float ty = c.y0 + 6 * k, th = 44 * k;
    float x = c.x0 + 14 * k;
    const char* tabs[5] = {"MUSIC", "TV", "MOVIES", "VIDEO", "RADIO"};
    const int tabIdx[5] = {0, 3, 4, 1, 2};
    for (int n = 0; n < 5; ++n) {
        const int i = tabIdx[n];
        const int hid = i < 3 ? 1 + i : 2 + i;
        Rect r{x, ty, x + 128 * k, ty + th};
        State s = tab_ == i ? State::Selected : (hot(hid, 0) ? State::Hover : State::Normal);
        button(bm, r, tabs[n], s, tab_ == i, 21 * k);
        addHit(r, hid);
        x += 134 * k;
    }
    if (!theatre_) {
        const bool busy = host_.pickBusy && host_.pickBusy();
        Rect r{x + 8 * k, ty, x + 8 * k + 190 * k, ty + th};
        button(bm, r, busy ? "CHOOSING..." : "ADD FOLDERS", hot(52, 0) ? State::Hover : State::Normal, false, 20 * k);
        addHit(r, 52);
        Rect m{r.x1 + 8 * k, ty, r.x1 + 8 * k + 130 * k, ty + th};
        button(bm, m, manage_ ? "DONE" : "MANAGE", manage_ ? State::Selected : (hot(51, 0) ? State::Hover : State::Normal), manage_, 20 * k);
        addHit(m, 51);
    }
    const bool searchable = tab_ != 2;
    webLoads_ = 0;
    if (searchable && !theatre_) {
        const Rect sr{c.x1 - 14 * k - 340 * k, ty, c.x1 - 14 * k, ty + th};
        card(bm, sr, searchFocus_ ? State::Selected : (hot(4, 0) ? State::Hover : State::Normal));
        addHit(sr, 4);
        const bool caret = searchFocus_ && (SDL_GetTicks() / 500) % 2 == 0;
        if (q_.empty() && !searchFocus_) label(bm, sr.x0 + 16 * k, sr.y0 + 30 * k, "SEARCH", 22 * k, FontFace::UI, t.muted);
        else label(bm, sr.x0 + 16 * k, sr.y0 + 30 * k, ellipsize(q_, 24 * k, FontFace::UI, sr.w() - 44 * k) + (caret ? "|" : ""), 24 * k, FontFace::UI, t.text);
    }

    // bar geometry first, so the scrolling region can end above it
    const float barH = 140 * k;
    const Rect barR{c.x0, c.y1 - barH, c.x1, c.y1};
    Rect view{c.x0, ty + th + 12 * k, c.x1, barR.y0 - 12 * k};
    const bool gridView = !theatre_ && !manage_ &&
                          ((tab_ == 0 && albumOpen_ < 0 && artistOpen_ < 0) || (tab_ == 3 && showOpen_ < 0) || (tab_ == 4 && movieOpen_ < 0));
    if (gridView) {
        // sort / filter strip above the scrolling grid
        const float sy = view.y0, sh = 42 * k;
        float sx = view.x0 + 14 * k;
        auto chip = [&](const std::string& txt, int id, bool on) {
            const float w = measureText(txt, 19 * k, FontFace::UIBold) + 36 * k;
            const Rect r{sx, sy, sx + w, sy + sh};
            button(bm, r, txt, on ? State::Selected : (hot(id, 0) ? State::Hover : State::Normal), on, 19 * k);
            addHit(r, id);
            sx += w + 10 * k;
        };
        if (tab_ == 0) {
            chip(musicByAlbum_ ? "VIEW: ALBUMS" : "VIEW: ARTISTS", 7, false);
            static const char* sm[2][4] = {{"SORT: A-Z", "SORT: Z-A", "SORT: MOST ALBUMS", "SORT: MOST TRACKS"}, {"SORT: A-Z", "SORT: YEAR", "SORT: ARTIST", "SORT: Z-A"}};
            chip(sm[musicByAlbum_ ? 1 : 0][sortMode_], 8, false);
        } else {
            static const char* sm[4] = {"SORT: A-Z", "SORT: RECENT", "SORT: YEAR", "SORT: RATING"};
            static const char* fm[4] = {"SHOW: ALL", "SHOW: IN PROGRESS", "SHOW: UNWATCHED", "SHOW: WATCHED"};
            chip(sm[sortMode_], 8, false);
            chip(fm[filterMode_], 9, filterMode_ != 0);
        }
        if (scrape_.offline()) label(bm, c.x1 - 14 * k - measureText("OFFLINE - USING SAVED INFO", 18 * k, FontFace::UI), sy + 28 * k, "OFFLINE - USING SAVED INFO", 18 * k, FontFace::UI, t.muted);
        else if (scrape_.pending() > 0) {
            size_t sd = 0, st2 = 0;
            scrape_.progress(sd, st2);
            const std::string msg = "FETCHING INFO  " + std::to_string(sd) + " / " + std::to_string(st2);
            const float mw = measureText(msg, 18 * k, FontFace::UI);
            label(bm, c.x1 - 14 * k - mw, sy + 20 * k, msg, 18 * k, FontFace::UI, t.accent);
            bar(bm, Rect{c.x1 - 14 * k - mw, sy + 28 * k, c.x1 - 14 * k, sy + 34 * k}, st2 ? static_cast<float>(sd) / st2 : 0.0f, t.accent);
        }
        view.y0 += sh + 10 * k;
    }

    // scrolling content is painted on a transparent layer and clipped to the view
    if (layer_.w != bm.w || layer_.h != bm.h) {
        layer_.w = bm.w;
        layer_.h = bm.h;
        layer_.rgba.assign(static_cast<size_t>(bm.w) * bm.h * 4, 0);
    } else {
        const int ya = std::max(0, static_cast<int>(view.y0)), yb = std::min(bm.h, static_cast<int>(view.y1) + 1);
        std::fill(layer_.rgba.begin() + static_cast<size_t>(ya) * bm.w * 4, layer_.rgba.begin() + static_cast<size_t>(yb) * bm.w * 4, 0);
    }
    Bitmap& L = layer_;
    const float gap = 20 * k, pad = 10 * k;
    float contentH = 0;
    auto onScreen = [&](float y0, float y1) { return y1 > view.y0 && y0 < view.y1; };
    const std::string q = lower(q_);

    auto emptyState = [&](const std::string& title, const std::string& body, bool buttons) {
        const float cy = (view.y0 + view.y1) * 0.5f - 40 * k;
        const float tw = measureText(title, 36 * k, FontFace::UIBold);
        label(L, (view.x0 + view.x1) * 0.5f - tw * 0.5f, cy, title, 36 * k, FontFace::UIBold, t.text);
        const float bw = measureText(body, 22 * k, FontFace::UI);
        label(L, (view.x0 + view.x1) * 0.5f - bw * 0.5f, cy + 40 * k, body, 22 * k, FontFace::UI, t.dim);
        if (buttons) {
            const float mx0 = (view.x0 + view.x1) * 0.5f;
            const Rect a{mx0 - 330 * k, cy + 70 * k, mx0 - 10 * k, cy + 118 * k}, b{mx0 + 10 * k, cy + 70 * k, mx0 + 330 * k, cy + 118 * k};
            button(L, a, "ADD MUSIC + VIDEOS", hot(50, 0) ? State::Hover : State::Normal, true, 22 * k);
            button(L, b, host_.pickBusy && host_.pickBusy() ? "CHOOSING..." : "BROWSE FOR FOLDERS", hot(52, 0) ? State::Hover : State::Normal, false, 22 * k);
            addHit(a, 50, 0, &view);
            addHit(b, 52, 0, &view);
        }
    };

    if (manage_ && !theatre_) {
        // ---- watched folders: each one can be removed, new ones added with the button above
        const std::vector<std::string> fl = host_.folders ? host_.folders() : std::vector<std::string>{};
        float y = view.y0 + pad;
        label(L, view.x0 + pad, y + 26 * k, "WATCHED FOLDERS", 26 * k, FontFace::UIBold, t.accent);
        label(L, view.x0 + pad + 300 * k, y + 26 * k, "MUSIC AND VIDEO IN THESE FOLDERS (AND BELOW) SHOWS UP IN THE LIBRARY", 18 * k, FontFace::UI, t.dim);
        y += 54 * k;
        if (fl.empty()) label(L, view.x0 + pad, y + 30 * k, "NO FOLDERS YET - USE ADD FOLDERS, OR DROP ONE ONTO THE WINDOW", 22 * k, FontFace::UI, t.dim);
        for (size_t i = 0; i < fl.size(); ++i) {
            const Rect row{view.x0 + pad, y, view.x1 - pad - 190 * k, y + 52 * k};
            const Rect rm{view.x1 - pad - 170 * k, y, view.x1 - pad, y + 52 * k};
            if (onScreen(y, y + 52 * k)) {
                card(L, row, State::Normal);
                label(L, row.x0 + 18 * k, row.y0 + 33 * k, ellipsize(fl[i], 22 * k, FontFace::UI, row.w() - 36 * k), 22 * k, FontFace::UI, t.text);
                button(L, rm, "REMOVE", hot(53, static_cast<int>(i)) ? State::Hover : State::Normal, false, 20 * k);
                addHit(rm, 53, static_cast<int>(i), &view);
            }
            y += 62 * k;
        }
        contentH = y - view.y0;
    } else if (theatre_) {
        // ---- video theatre: the picture fills the view, a click pauses
        fillRound(L, view.x0, view.y0, view.x1, view.y1, 10 * k, Color8{0, 0, 0, 255});
        if (frame_.w > 0 && frame_.h > 0) {
            holeFit(L, frame_.w, frame_.h, view, vx_, vy_, vw_, vh_);
        } else {
            const std::string s = now_.loading ? "OPENING" : "LOADING VIDEO";
            label(L, (view.x0 + view.x1) * 0.5f - measureText(s, 26 * k, FontFace::UIBold) * 0.5f, (view.y0 + view.y1) * 0.5f, s, 26 * k, FontFace::UIBold, t.dim);
        }
        addHit(view, 10);
        const Rect br{view.x0 + 14 * k, view.y0 + 14 * k, view.x0 + 194 * k, view.y0 + 58 * k};
        button(L, br, "LIBRARY", hot(60, 0) ? State::Hover : State::Normal, false, 20 * k);
        addHit(br, 60);
        const Rect fr{br.x1 + 10 * k, br.y0, br.x1 + 250 * k, br.y1};
        button(L, fr, "FULLSCREEN", hot(63, 0) ? State::Hover : State::Normal, false, 20 * k);
        addHit(fr, 63);
        contentH = 0;
    } else if (tab_ == 0 && albumOpen_ >= 0 && albumOpen_ < static_cast<int>(albums_.size())) {
        // ---- album detail: fixed info column on the left, scrolling track list on the right
        Album& al = albums_[albumOpen_];
        const float lw = 340 * k;
        const Rect cover{view.x0 + pad, view.y0 + pad, view.x0 + pad + lw, view.y0 + pad + lw};
        if (const Bitmap* art = artFor(al)) drawImage(L, *art, cover, 10 * k);
        else drawPlaceholder(L, cover, 10 * k, al.key, al.title);
        float ly = cover.y1 + 34 * k;
        const auto lines = wrapText(al.title, 32 * k, FontFace::UIBold, lw, 2);
        for (const std::string& ln : lines) {
            label(L, cover.x0, ly, ln, 32 * k, FontFace::UIBold, t.text);
            ly += 38 * k;
        }
        label(L, cover.x0, ly, ellipsize(al.artist.empty() ? "UNKNOWN ARTIST" : al.artist, 24 * k, FontFace::UI, lw), 24 * k, FontFace::UI, t.accent);
        ly += 30 * k;
        std::string meta = (al.year ? std::to_string(al.year) + "  /  " : std::string()) + std::to_string(al.items.size()) + " TRACKS";
        const Info* ai = al.artist.empty() ? nullptr : info(MediaScrape::albumKey(al.artist, al.title));
        if (!ai && !al.artist.empty()) scrape_.requestAlbum(al.artist, al.title);
        if (ai && ai->found) {
            if (!ai->year.empty() && !al.year) meta = ai->year + "  /  " + std::to_string(al.items.size()) + " TRACKS";
            label(L, cover.x0, ly, meta, 20 * k, FontFace::UI, t.dim);
            ly += 26 * k;
            const std::string extra = joinDot({ai->genres, ai->extra});
            if (!extra.empty()) {
                label(L, cover.x0, ly, ellipsize(extra, 18 * k, FontFace::UI, lw), 18 * k, FontFace::UI, t.accent);
                ly += 6 * k;
            }
        } else {
            label(L, cover.x0, ly, meta, 20 * k, FontFace::UI, t.dim);
        }
        ly += 22 * k;
        const float bw = (lw - 2 * 10 * k) / 3;
        const char* bl[3] = {"PLAY", "SHUFFLE", "BACK"};
        const int bid[3] = {23, 24, 22};
        for (int i = 0; i < 3; ++i) {
            const Rect r{cover.x0 + i * (bw + 10 * k), ly, cover.x0 + i * (bw + 10 * k) + bw, ly + 44 * k};
            button(L, r, bl[i], hot(bid[i], 0) ? State::Hover : State::Normal, i == 0, 20 * k);
            addHit(r, bid[i], 0, &view);
        }

        const float rowH = 50 * k, lx0 = view.x0 + lw + 50 * k, lx1 = view.x1 - pad;
        float y = view.y0 + pad - scroll_;
        PCtx lc{L, view, k, pad, gap, t,
                [&](const Rect& r, int id, int arg) { addHit(r, id, arg, &view); },
                [&](int id, int arg) { return hot(id, arg); },
                [&](const std::string&, const std::string&) {}};
        const bool showLyr = lyricsItem_ >= 0 && lyricsItem_ < static_cast<int>(items_.size());
        if (showLyr) {
            contentH = drawLyrics(lc, lx0, lx1);
            y = view.y0;
        }
        for (size_t i = 0; i < al.items.size() && !showLyr; ++i) {
            const Item& it = items_[al.items[i]];
            const Rect r{lx0, y, lx1, y + rowH - 4 * k};
            if (onScreen(r.y0, r.y1)) {
                const bool playing = now_.active && !now_.live && now_.path == it.t.path;
                const bool hv = hot(25, static_cast<int>(i));
                if (playing) fillRound(L, r.x0, r.y0, r.x1, r.y1, 6 * k, withAlpha(t.accent, 56));
                else if (hv) fillRound(L, r.x0, r.y0, r.x1, r.y1, 6 * k, withAlpha(t.accent, 26));
                if (playing) fillRect(L, r.x0, r.y0 + 6 * k, r.x0 + 4 * k, r.y1 - 6 * k, t.accent);
                const float base = r.y0 + 31 * k;
                const std::string num = it.m.track > 0 ? std::to_string(it.m.track) : std::to_string(i + 1);
                if (hv && !playing) triangle(L, r.x0 + 32 * k, (r.y0 + r.y1) * 0.5f, 16 * k, true, t.accent);
                else if (playing && now_.playing) {
                    for (int b = 0; b < 3; ++b) {
                        const float bh = (6 + 10 * std::fabs(std::sin(clockT_ * 5 + b * 1.7))) * k;
                        fillRect(L, r.x0 + (22 + b * 7) * k, (r.y0 + r.y1) * 0.5f + 9 * k - bh, r.x0 + (27 + b * 7) * k, (r.y0 + r.y1) * 0.5f + 9 * k, t.accent);
                    }
                } else labelRight(L, r.x0 + 46 * k, base, num, 22 * k, FontFace::UI, playing ? t.accent : t.muted);
                const std::string dur = it.m.seconds > 0 ? clock(it.m.seconds) : std::string();
                const float dw = measureText(dur, 20 * k, FontFace::UI);
                label(L, r.x0 + 70 * k, base, ellipsize(it.title, 24 * k, FontFace::UI, r.w() - 70 * k - dw - 150 * k), 24 * k, FontFace::UI, playing ? t.accent : t.text);
                labelRight(L, r.x1 - 16 * k, base, dur, 20 * k, FontFace::UI, t.dim);
                addHit(r, 25, static_cast<int>(i), &view);
                {
                    const Rect lr{r.x1 - 16 * k - dw - 108 * k, r.y0 + 8 * k, r.x1 - 16 * k - dw - 16 * k, r.y1 - 12 * k};
                    const bool lh = hot(93, static_cast<int>(al.items[i]));
                    button(L, lr, "LYRICS", lh ? State::Hover : State::Normal, false, 15 * k);
                    addHit(lr, 93, static_cast<int>(al.items[i]), &view);
                }
            }
            y += rowH;
        }
        if (!showLyr) contentH = y + scroll_ - view.y0 + pad;
    } else if (tab_ == 3 || tab_ == 4 || (tab_ == 0 && !musicByAlbum_)) {
        PCtx pc{L, view, k, pad, gap, t,
                [&](const Rect& r, int id, int arg) { addHit(r, id, arg, &view); },
                [&](int id, int arg) { return hot(id, arg); },
                [&](const std::string& ti, const std::string& body) { emptyState(ti, body, ti.rfind("NO ", 0) == 0 && ti != "NO MATCHES"); }};
        if (tab_ == 3) contentH = showOpen_ >= 0 && showOpen_ < static_cast<int>(shows_.size()) ? paintShowPage(pc) : paintShowGrid(pc);
        else if (tab_ == 4) contentH = movieOpen_ >= 0 && movieOpen_ < static_cast<int>(movies_.size()) ? paintMoviePage(pc) : paintMovieGrid(pc);
        else contentH = artistOpen_ >= 0 && artistOpen_ < static_cast<int>(artists_.size()) ? paintArtistPage(pc) : paintArtistGrid(pc);
    } else if (tab_ == 0) {
        // ---- album grid
        std::vector<int> vis;
        for (size_t i = 0; i < albums_.size(); ++i)
            if (albumMatches(albums_[i])) vis.push_back(static_cast<int>(i));
        if (vis.empty()) {
            if (!q.empty()) emptyState("NO MATCHES", "NOTHING IN YOUR MUSIC MATCHES THE SEARCH", false);
            else if (libScanning_) emptyState("SCANNING YOUR MUSIC", "ALBUMS APPEAR HERE AS THEY ARE FOUND", false);
            else emptyState("NO MUSIC YET", "ADD YOUR MUSIC AND VIDEO FOLDERS, OR DROP A FOLDER ONTO THE WINDOW", true);
        } else {
            const int cols = std::max(2, static_cast<int>((view.w() - 2 * pad + gap) / (220 * k + gap)));
            const float tw = (view.w() - 2 * pad - gap * (cols - 1)) / cols;
            const float rowH = tw + 70 * k + gap;
            for (size_t n = 0; n < vis.size(); ++n) {
                const int ai = vis[n];
                Album& al = albums_[ai];
                const int col = static_cast<int>(n) % cols, row = static_cast<int>(n) / cols;
                const float x0 = view.x0 + pad + col * (tw + gap), y0 = view.y0 + pad + row * rowH - scroll_;
                if (!onScreen(y0, y0 + rowH)) continue;
                const Rect cover{x0, y0, x0 + tw, y0 + tw};
                const bool hv = hot(20, ai) || hot(21, ai);
                if (const Bitmap* art = artFor(al)) drawImage(L, *art, cover, 8 * k);
                else drawPlaceholder(L, cover, 8 * k, al.key, al.title);
                const bool playingAlbum = now_.active && !now_.live && nowAlbum_ == ai;
                if (hv) fillRound(L, cover.x0, cover.y0, cover.x1, cover.y1, 8 * k, Color8{0, 0, 0, 90});
                if (hv || playingAlbum) {
                    strokeRect(L, cover.x0, cover.y0, cover.x1, cover.y1, 2 * k, withAlpha(t.accent, hv ? 220 : 140));
                }
                if (hv) {
                    const float pr = 34 * k, pcx = cover.x1 - pr - 12 * k, pcy = cover.y1 - pr - 12 * k;
                    const bool ph2 = hot(21, ai);
                    dot(L, pcx, pcy, pr, ph2 ? t.accent : withAlpha(t.accent, 215));
                    triangle(L, pcx, pcy, 28 * k, true, Color8{8, 13, 20, 255});
                    addHit(Rect{pcx - pr, pcy - pr, pcx + pr, pcy + pr}, 21, ai, &view);
                }
                addHit(Rect{x0, y0, x0 + tw, y0 + tw + 66 * k}, 20, ai, &view);
                label(L, x0, y0 + tw + 28 * k, ellipsize(al.title, 22 * k, FontFace::UIBold, tw), 22 * k, FontFace::UIBold, hv || playingAlbum ? t.accent : t.text);
                label(L, x0, y0 + tw + 54 * k, ellipsize(al.artist.empty() ? "UNKNOWN ARTIST" : al.artist, 19 * k, FontFace::UI, tw), 19 * k, FontFace::UI, t.dim);
            }
            contentH = pad * 2 + ((static_cast<int>(vis.size()) + cols - 1) / cols) * rowH;
        }
    } else if (tab_ == 1) {
        // ---- video: a section per folder
        std::vector<int> vis;
        for (size_t i = 0; i < vids_.size(); ++i)
            if (vids_[i].vc.kind == VidClass::Other && (q.empty() || lower(vids_[i].t.title).find(q) != std::string::npos || lower(vids_[i].t.folder).find(q) != std::string::npos))
                vis.push_back(static_cast<int>(i));
        if (vis.empty()) {
            if (!q.empty()) emptyState("NO MATCHES", "NO VIDEO MATCHES THE SEARCH", false);
            else if (libScanning_) emptyState("SCANNING YOUR VIDEOS", "POSTERS APPEAR HERE AS THEY ARE FOUND", false);
            else emptyState("NO VIDEOS YET", "ADD YOUR MUSIC AND VIDEO FOLDERS, OR DROP A FOLDER ONTO THE WINDOW", true);
        } else {
            const int cols = std::max(2, static_cast<int>((view.w() - 2 * pad + gap) / (300 * k + gap)));
            const float tw = (view.w() - 2 * pad - gap * (cols - 1)) / cols, thh = tw * 9.0f / 16.0f;
            const float textH = 66 * k;
            float y = view.y0 + pad - scroll_;
            size_t n = 0;
            while (n < vis.size()) {
                const std::string dir = vids_[vis[n]].dir;
                const std::string name = vids_[vis[n]].t.folder.empty() ? "VIDEOS" : vids_[vis[n]].t.folder;
                size_t m = n;
                while (m < vis.size() && vids_[vis[m]].dir == dir) ++m;
                const float hy = y;
                if (onScreen(hy, hy + 52 * k)) {
                    label(L, view.x0 + pad, hy + 32 * k, ellipsize(name, 26 * k, FontFace::UIBold, view.w() * 0.6f), 26 * k, FontFace::UIBold, t.accent);
                    const std::string cnt = std::to_string(m - n) + " VIDEOS";
                    const Rect pa{view.x1 - pad - 190 * k, hy + 2 * k, view.x1 - pad, hy + 44 * k};
                    button(L, pa, "PLAY ALL", hot(31, vis[n]) ? State::Hover : State::Normal, false, 20 * k);
                    addHit(pa, 31, vis[n], &view);
                    labelRight(L, pa.x0 - 16 * k, hy + 30 * k, cnt, 20 * k, FontFace::UI, t.dim);
                    fillRect(L, view.x0 + pad, hy + 46 * k, view.x1 - pad, hy + 47 * k, withAlpha(t.accent, 60));
                }
                y += 58 * k;
                for (size_t i = n; i < m; ++i) {
                    const int col = static_cast<int>(i - n) % cols, row = static_cast<int>(i - n) / cols;
                    const float x0 = view.x0 + pad + col * (tw + gap), y0 = y + row * (thh + textH + gap * 0.5f);
                    if (!onScreen(y0, y0 + thh + textH)) continue;
                    Vid& v = vids_[vis[i]];
                    const bool hv = hot(30, vis[i]);
                    const Rect pr{x0, y0, x0 + tw, y0 + thh};
                    if (const Bitmap* th2 = thumbFor(v)) drawImage(L, *th2, pr, 8 * k);
                    else drawPlaceholder(L, pr, 8 * k, v.t.title, v.t.title);
                    const bool playingNow = now_.active && now_.path == v.t.path;
                    if (hv) fillRound(L, pr.x0, pr.y0, pr.x1, pr.y1, 8 * k, Color8{0, 0, 0, 90});
                    if (hv || playingNow) strokeRect(L, pr.x0, pr.y0, pr.x1, pr.y1, 2 * k, withAlpha(t.accent, hv ? 220 : 140));
                    if (hv) {
                        const float r2 = 30 * k, cx = (pr.x0 + pr.x1) * 0.5f, cy = (pr.y0 + pr.y1) * 0.5f;
                        dot(L, cx, cy, r2, withAlpha(t.accent, 225));
                        triangle(L, cx, cy, 26 * k, true, Color8{8, 13, 20, 255});
                    }
                    const auto ln = wrapText(v.t.title, 20 * k, FontFace::UIBold, tw, 2);
                    for (size_t li = 0; li < ln.size(); ++li)
                        label(L, x0, y0 + thh + (26 + 26 * li) * k, ln[li], 20 * k, FontFace::UIBold, hv || playingNow ? t.accent : t.text);
                    addHit(Rect{x0, y0, x0 + tw, y0 + thh + textH}, 30, vis[i], &view);
                }
                const int rows = (static_cast<int>(m - n) + cols - 1) / cols;
                y += rows * (thh + textH + gap * 0.5f) + 14 * k;
                n = m;
            }
            contentH = y + scroll_ - view.y0 + pad;
        }
    } else {
        // ---- radio
        const int cols = std::max(2, static_cast<int>((view.w() - 2 * pad + gap) / (360 * k + gap)));
        const float tw = (view.w() - 2 * pad - gap * (cols - 1)) / cols, tH = 112 * k;
        float y = view.y0 + pad - scroll_;
        if (onScreen(y, y + 36 * k))
            label(L, view.x0 + pad, y + 24 * k, "LIVE INTERNET RADIO. ADD YOUR OWN IN RADIO.TXT, ONE PER LINE: NAME|URL|GENRE", 18 * k, FontFace::UI, t.dim);
        y += 48 * k;
        for (size_t i = 0; i < stations_.size(); ++i) {
            const int col = static_cast<int>(i) % cols, row = static_cast<int>(i) / cols;
            const float x0 = view.x0 + pad + col * (tw + gap), y0 = y + row * (tH + gap);
            if (!onScreen(y0, y0 + tH)) continue;
            const Station& s = stations_[i];
            const bool hv = hot(40, static_cast<int>(i));
            const bool playingNow = now_.active && now_.live && now_.title == s.name;
            const Rect r{x0, y0, x0 + tw, y0 + tH};
            drawPlaceholder(L, r, 10 * k, s.name, "");
            fillRound(L, r.x0 + 1, r.y0 + 1, r.x1 - 1, r.y1 - 1, 10 * k, Color8{8, 13, 20, 90});
            if (hv || playingNow) strokeRect(L, r.x0, r.y0, r.x1, r.y1, 2 * k, withAlpha(t.accent, hv ? 220 : 160));
            label(L, r.x0 + 22 * k, r.y0 + 46 * k, ellipsize(s.name, 28 * k, FontFace::UIBold, tw - 120 * k), 28 * k, FontFace::UIBold, hv || playingNow ? t.accent : t.text);
            label(L, r.x0 + 22 * k, r.y0 + 78 * k, ellipsize(s.genre, 20 * k, FontFace::UI, tw - 120 * k), 20 * k, FontFace::UI, t.dim);
            const float cx = r.x1 - 50 * k, cy = (r.y0 + r.y1) * 0.5f;
            dot(L, cx, cy, 28 * k, hv || playingNow ? t.accent : withAlpha(t.accent, 90));
            if (playingNow && now_.playing) {
                for (int b = 0; b < 3; ++b) {
                    const float bh = (8 + 14 * std::fabs(std::sin(clockT_ * 6 + b * 1.9))) * k;
                    fillRect(L, cx + (-10 + b * 8) * k, cy + 10 * k - bh, cx + (-5 + b * 8) * k, cy + 10 * k, Color8{8, 13, 20, 255});
                }
            } else triangle(L, cx, cy, 22 * k, true, hv || playingNow ? Color8{8, 13, 20, 255} : t.text);
            addHit(r, 40, static_cast<int>(i), &view);
        }
        const int rows = (static_cast<int>(stations_.size()) + cols - 1) / cols;
        contentH = 48 * k + pad * 2 + rows * (tH + gap);
    }

    scrollMax_ = std::max(0.0f, contentH - view.h());
    if (scrollTarget_ > scrollMax_) scrollTarget_ = scrollMax_;
    if (scroll_ > scrollMax_) scroll_ = scrollMax_;

    // composite the layer inside the view
    {
        const int ya = std::max(0, static_cast<int>(view.y0)), yb = std::min(bm.h, static_cast<int>(view.y1)), xa = std::max(0, static_cast<int>(view.x0)), xb = std::min(bm.w, static_cast<int>(view.x1));
        for (int yy = ya; yy < yb; ++yy)
            for (int xx = xa; xx < xb; ++xx) {
                const uint8_t* s = &L.rgba[(static_cast<size_t>(yy) * bm.w + xx) * 4];
                if (s[3] == 0) continue;
                uint8_t* d = &bm.rgba[(static_cast<size_t>(yy) * bm.w + xx) * 4];
                if (s[3] == 255) {
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
                } else {
                    blendPx(d, Color8{s[0], s[1], s[2], s[3]}, 1.0f);
                }
            }
        // the window background sits under the layer: re-cut the video hole so the picture shows at full brightness
        if (theatre_ && vw_ > 0 && vh_ > 0)
            for (int yy = std::max(0, vy_); yy < std::min(bm.h, vy_ + vh_); ++yy)
                for (int xx = std::max(0, vx_); xx < std::min(bm.w, vx_ + vw_); ++xx) {
                    const size_t o = (static_cast<size_t>(yy) * bm.w + xx) * 4;
                    if (L.rgba[o + 3] == 0) std::memset(&bm.rgba[o], 0, 4);
                }
        // scroll indicator
        if (scrollMax_ > 1) {
            const float trackH = view.h(), th2 = std::max(40 * k, trackH * view.h() / (contentH + 1)), ty2 = view.y0 + (trackH - th2) * (scroll_ / scrollMax_);
            fillRound(bm, view.x1 - 6 * k, ty2, view.x1 - 2 * k, ty2 + th2, 2 * k, withAlpha(t.accent, 120));
        }
    }

    // ---- now playing bar
    card(bm, barR, State::Normal);
    const float bp = 16 * k, bw = barR.w();
    const float artS = barR.h() - 2 * bp;
    const Rect artR{barR.x0 + bp, barR.y0 + bp, barR.x0 + bp + artS, barR.y0 + bp + artS};
    if (now_.active) {
        if (nowPathSeen_ != now_.path) {
            nowPathSeen_ = now_.path;
            nowAlbum_ = -1;
            for (size_t a = 0; a < albums_.size() && nowAlbum_ < 0; ++a)
                for (int i : albums_[a].items)
                    if (items_[i].t.path == now_.path) { nowAlbum_ = static_cast<int>(a); break; }
        }
        const Bitmap* img = nullptr;
        if (now_.video) {
            auto f = thumbCache_.find(now_.path);
            if (f != thumbCache_.end()) img = &f->second;
        } else if (nowAlbum_ >= 0) {
            img = artFor(albums_[nowAlbum_]);
        }
        if (img) drawImage(bm, *img, artR, 6 * k);
        else drawPlaceholder(bm, artR, 6 * k, now_.title, now_.title);
    } else {
        fillRound(bm, artR.x0, artR.y0, artR.x1, artR.y1, 6 * k, withAlpha(t.accent, 22));
        strokeRect(bm, artR.x0, artR.y0, artR.x1, artR.y1, 1.5f * k, withAlpha(t.accent, 70));
    }
    if (now_.active && !now_.live) addHit(artR, 61);
    const float tx0 = artR.x1 + 18 * k, tw0 = bw * 0.29f - artS;
    if (now_.active) {
        label(bm, tx0, barR.y0 + 54 * k, ellipsize(now_.title, 26 * k, FontFace::UIBold, tw0), 26 * k, FontFace::UIBold, t.text);
        std::string line2;
        if (now_.live) line2 = "LIVE RADIO";
        else if (now_.video) line2 = "VIDEO";
        else if (nowAlbum_ >= 0) line2 = albums_[nowAlbum_].artist + (albums_[nowAlbum_].artist.empty() ? "" : "  /  ") + albums_[nowAlbum_].title;
        if (now_.loading) line2 = "CONNECTING";
        label(bm, tx0, barR.y0 + 84 * k, ellipsize(line2, 20 * k, FontFace::UI, tw0), 20 * k, FontFace::UI, t.dim);
    } else {
        label(bm, tx0, barR.y0 + 58 * k, "NOTHING PLAYING", 24 * k, FontFace::UIBold, t.muted);
        label(bm, tx0, barR.y0 + 88 * k, "PICK AN ALBUM, A VIDEO OR A STATION", 18 * k, FontFace::UI, t.muted);
    }

    // transport
    const float cx0 = barR.x0 + bw * 0.31f, cx1 = barR.x0 + bw * 0.71f, cxm = (cx0 + cx1) * 0.5f, cyT = barR.y0 + 18 * k + 30 * k;
    const bool canMove = now_.active && !now_.live;
    const bool canToggle = now_.active;
    {
        const Color8 on = t.text, off = t.muted;
        const float pr = 30 * k;
        dot(bm, cxm, cyT, pr, canToggle ? (hot(10, 0) ? t.accent : withAlpha(t.accent, 205)) : withAlpha(t.accent, 40));
        if (now_.playing && canToggle) {
            fillRect(bm, cxm - 10 * k, cyT - 12 * k, cxm - 3 * k, cyT + 12 * k, Color8{8, 13, 20, 255});
            fillRect(bm, cxm + 3 * k, cyT - 12 * k, cxm + 10 * k, cyT + 12 * k, Color8{8, 13, 20, 255});
        } else {
            triangle(bm, cxm, cyT, 26 * k, true, canToggle ? Color8{8, 13, 20, 255} : off);
        }
        if (canToggle) addHit(Rect{cxm - pr, cyT - pr, cxm + pr, cyT + pr}, 10);
        const float ox = 86 * k;
        const Color8 pc = canMove ? (hot(11, 0) ? t.accent : on) : off, nc = canMove ? (hot(12, 0) ? t.accent : on) : off;
        triangle(bm, cxm - ox, cyT, 22 * k, false, pc);
        fillRect(bm, cxm - ox - 14 * k, cyT - 11 * k, cxm - ox - 10 * k, cyT + 11 * k, pc);
        triangle(bm, cxm + ox, cyT, 22 * k, true, nc);
        fillRect(bm, cxm + ox + 10 * k, cyT - 11 * k, cxm + ox + 14 * k, cyT + 11 * k, nc);
        if (canMove) {
            addHit(Rect{cxm - ox - 24 * k, cyT - 24 * k, cxm - ox + 24 * k, cyT + 24 * k}, 11);
            addHit(Rect{cxm + ox - 24 * k, cyT - 24 * k, cxm + ox + 24 * k, cyT + 24 * k}, 12);
        }
        const Rect sh{cxm - 220 * k, cyT - 20 * k, cxm - 140 * k, cyT + 20 * k};
        const Rect rp{cxm + 140 * k, cyT - 20 * k, cxm + 220 * k, cyT + 20 * k};
        button(bm, sh, "SHUF", now_.shuffle ? State::Selected : (hot(13, 0) ? State::Hover : State::Normal), now_.shuffle, 17 * k);
        button(bm, rp, now_.repeat == 2 ? "RPT 1" : "RPT", now_.repeat ? State::Selected : (hot(14, 0) ? State::Hover : State::Normal), now_.repeat != 0, 17 * k);
        addHit(sh, 13);
        addHit(rp, 14);
    }
    // seek
    {
        const float sy = barR.y1 - bp - 30 * k;
        const Rect sk{cx0 + 64 * k, sy, cx1 - 64 * k, sy + 10 * k};
        const bool seekable = canMove && now_.dur > 0;
        skK_ = k; skX0_ = cx0; skX1_ = cx1; skY_ = sy; skSeekable_ = seekable;
        skAccent_ = t.accent; skText_ = t.text; skDim_ = t.dim;
        {
            const int sx0 = std::max(0, static_cast<int>(cx0)), sx1 = std::min(bm.w, static_cast<int>(cx1) + 2);
            const int sy0 = std::max(0, static_cast<int>(sy - 30 * k)), sy1 = std::min(bm.h, static_cast<int>(sy + 26 * k));
            skValid_ = false;
            if (sx1 > sx0 && sy1 > sy0) {
                skPx_ = {sx0, sy0, sx1 - sx0, sy1 - sy0};
                skBack_.resize(static_cast<size_t>(skPx_.w) * skPx_.h * 4);
                for (int yy = 0; yy < skPx_.h; ++yy)
                    std::memcpy(&skBack_[static_cast<size_t>(yy) * skPx_.w * 4], &bm.rgba[(static_cast<size_t>(sy0 + yy) * bm.w + sx0) * 4], static_cast<size_t>(skPx_.w) * 4);
                skW_ = bm.w;
                skH_ = bm.h;
                skValid_ = true;
            }
        }
        drawSeek(bm);
        if (seekable) addHit(Rect{sk.x0, sk.y0 - 14 * k, sk.x1, sk.y1 + 14 * k}, 15);
    }
    // spectrum + volume
    {
        const float rx0 = barR.x0 + bw * 0.74f, rx1 = barR.x1 - bp;
        const Rect sp{rx0, barR.y0 + bp, rx1, barR.y1 - bp - 44 * k};
        // Snapshot what sits behind the bars so they can be redrawn alone, without repainting the whole window.
        spRect_ = Rect{sp.x0, sp.y0 - 4 * k, sp.x1, sp.y1};
        spK_ = k;
        spA_ = t.accent;
        spB_ = t.alt;
        spT_ = t.text;
        snapshotBackdrop(bm);
        drawBars(bm, sp);
        addHit(sp, 17);
        label(bm, rx0, barR.y1 - bp - 8 * k, "VOL", 18 * k, FontFace::UIBold, t.dim);
        const Rect vr{rx0 + 60 * k, barR.y1 - bp - 26 * k, rx1, barR.y1 - bp - 4 * k};
        slider(bm, vr, std::clamp(now_.vol, 0.0f, 1.0f), hot(16, 0) ? State::Hover : State::Normal);
        addHit(Rect{vr.x0, vr.y0 - 8 * k, vr.x1, vr.y1 + 8 * k}, 16);
    }

    // hover may have moved under the cursor (scrolling, new layout)
    if (mx() >= 0) {
        const int h = hitAt(mx(), my());
        if (h != prevHover) {
            hover_ = h;
            invalidate();
        }
    }
}

// ===== Library views: TV shows, movies, artists, with web-scraped posters, info and watch progress =====

void MediaApp::groupLibrary() {
    std::string showKeyOpen, moviePathOpen;
    if (showOpen_ >= 0 && showOpen_ < static_cast<int>(shows_.size())) showKeyOpen = shows_[showOpen_].key;
    if (movieOpen_ >= 0 && movieOpen_ < static_cast<int>(movies_.size()) && movies_[movieOpen_] < static_cast<int>(vids_.size())) moviePathOpen = vids_[movies_[movieOpen_]].t.path;
    shows_.clear();
    movies_.clear();
    std::map<std::string, int> idx;
    for (size_t i = 0; i < vids_.size(); ++i) {
        const VidClass& vc = vids_[i].vc;
        if (vc.kind == VidClass::Show) {
            const std::string key = MediaScrape::showKey(vc.name);
            auto f = idx.find(key);
            if (f == idx.end()) {
                Show s;
                s.name = vc.name;
                s.key = key;
                f = idx.emplace(key, static_cast<int>(shows_.size())).first;
                shows_.push_back(std::move(s));
            }
            shows_[f->second].eps.push_back(static_cast<int>(i));
        } else if (vc.kind == VidClass::Movie) {
            movies_.push_back(static_cast<int>(i));
        }
    }
    for (Show& s : shows_)
        std::sort(s.eps.begin(), s.eps.end(), [&](int a, int b) {
            const VidClass &x = vids_[a].vc, &y = vids_[b].vc;
            if (x.season != y.season) return x.season < y.season;
            if (x.episode != y.episode) return x.episode < y.episode;
            return natCmp(vids_[a].t.title, vids_[b].t.title) < 0;
        });
    std::sort(shows_.begin(), shows_.end(), [](const Show& a, const Show& b) { return natCmp(a.name, b.name) < 0; });
    std::sort(movies_.begin(), movies_.end(), [&](int a, int b) { return natCmp(vids_[a].vc.name, vids_[b].vc.name) < 0; });
    showOpen_ = movieOpen_ = -1;
    for (size_t i = 0; i < shows_.size(); ++i)
        if (!showKeyOpen.empty() && shows_[i].key == showKeyOpen) showOpen_ = static_cast<int>(i);
    for (size_t i = 0; i < movies_.size(); ++i)
        if (!moviePathOpen.empty() && vids_[movies_[i]].t.path == moviePathOpen) movieOpen_ = static_cast<int>(i);
    progress_ = db_.allProgress();
}

Progress MediaApp::prog(const std::string& path) const {
    auto f = progress_.find(path);
    return f == progress_.end() ? Progress() : f->second;
}

long long MediaApp::lastPlayed(const Show& s) const {
    long long l = 0;
    for (int v : s.eps) l = std::max(l, prog(vids_[v].t.path).last);
    return l;
}

// The episode a CONTINUE button should start: the one in progress, else the one after the last finished, else the first.
int MediaApp::resumeIndex(const Show& s) const {
    int best = -1;
    long long bl = 0;
    for (size_t i = 0; i < s.eps.size(); ++i) {
        const Progress p = prog(vids_[s.eps[i]].t.path);
        if (p.any && p.last >= bl) { bl = p.last; best = static_cast<int>(i); }
    }
    if (best < 0) return s.eps.empty() ? -1 : s.eps[0];
    const Progress p = prog(vids_[s.eps[best]].t.path);
    if (!p.watched) return s.eps[best];
    if (best + 1 < static_cast<int>(s.eps.size())) return s.eps[best + 1];
    return s.eps[0];
}

const Info* MediaApp::info(const std::string& key) {
    auto f = infoCache_.find(key);
    if (f == infoCache_.end()) {
        InfoEntry e;
        e.have = db_.getInfo(key, e.i);
        f = infoCache_.emplace(key, e).first;
    }
    return f->second.have ? &f->second.i : nullptr;
}

const Bitmap* MediaApp::webBitmap(const std::string& url, int w, int h) {
    if (url.empty()) return nullptr;
    const std::string key = url + "|" + std::to_string(w) + "x" + std::to_string(h);
    auto f = webCache_.find(key);
    if (f != webCache_.end()) return f->second.w > 0 ? &f->second : nullptr;
    auto p = imgPath_.find(key);
    if (p == imgPath_.end()) p = imgPath_.emplace(key, scrape_.image(url, w, h)).first;
    if (p->second.empty()) return nullptr;
    if (webLoads_ >= 3) {  // decode a few per frame so scrolling stays smooth
        invalidate();
        return nullptr;
    }
    ++webLoads_;
    Bitmap bmp;
    std::ifstream in(std::filesystem::u8path(p->second), std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int iw = 0, ih = 0, n = 0;
    unsigned char* px = data.empty() ? nullptr : stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &iw, &ih, &n, 4);
    if (px) {
        bmp.w = iw;
        bmp.h = ih;
        bmp.rgba.assign(px, px + static_cast<size_t>(iw) * ih * 4);
        stbi_image_free(px);
    }
    auto ins = webCache_.emplace(key, std::move(bmp)).first;
    return ins->second.w > 0 ? &ins->second : nullptr;
}

// Resume points are written every few seconds while a video plays; finishing 92% of it marks it watched.
void MediaApp::recordProgress() {
    if (!seekPath_.empty()) {
        if (now_.active && now_.path == seekPath_ && now_.dur > 0 && host_.seek) {
            host_.seek(seekTo_);
            seekPath_.clear();
        } else if (clockT_ - seekWait_ > 20) {
            seekPath_.clear();
        }
    }
    if (!(now_.active && now_.video && !now_.live && now_.dur > 1)) return;
    if (now_.path != recPath_) {
        recPath_ = now_.path;
        recT_ = clockT_;
        return;
    }
    if (clockT_ - recT_ < 3.0) return;
    recT_ = clockT_;
    const Progress old = prog(now_.path);
    const bool finished = now_.pos > now_.dur * 0.92;
    if (!now_.playing && old.any && std::fabs(old.pos - now_.pos) < 1.0) return;
    db_.setProgress(now_.path, finished ? 0.0 : now_.pos, now_.dur, old.watched || finished);
    progress_[now_.path] = db_.progress(now_.path);
}

void MediaApp::playVids(const std::vector<int>& list, int start) {
    if (list.empty() || !host_.play) return;
    std::vector<std::string> paths;
    for (int i : list) paths.push_back(vids_[i].t.path);
    start = std::clamp(start, 0, static_cast<int>(paths.size()) - 1);
    const Progress p = prog(paths[start]);
    seekPath_.clear();
    if (p.any && !p.watched && p.pos > 20 && p.dur > 0 && p.pos < p.dur * 0.95) {
        seekPath_ = paths[start];
        seekTo_ = p.pos;
        seekWait_ = clockT_;
    }
    host_.play(paths, start);
    theatre_ = true;
    videoSeen_ = false;
    frame_ = Bitmap();
}

bool MediaApp::filterOk(const Progress& p) const {
    switch (filterMode_) {
        case 1: return p.any && !p.watched && p.pos > 0;
        case 2: return !p.watched && p.pos <= 0;
        case 3: return p.watched;
        default: return true;
    }
}

namespace {
// Poster tile: image or placeholder, hover glow, optional progress strip along the bottom.
void posterTile(Bitmap& L, const ui::Rect& pr, float k, const ui::Tokens& t, const Bitmap* img, const std::string& seed, const std::string& name, bool hv, float frac, bool done) {
    using namespace ui;
    if (img) drawImage(L, *img, pr, 8 * k);
    else drawPlaceholder(L, pr, 8 * k, seed, name);
    if (hv) fillRound(L, pr.x0, pr.y0, pr.x1, pr.y1, 8 * k, Color8{0, 0, 0, 80});
    if (frac > 0.0f && !done) {
        fillRect(L, pr.x0 + 8 * k, pr.y1 - 14 * k, pr.x1 - 8 * k, pr.y1 - 8 * k, Color8{0, 0, 0, 170});
        fillRect(L, pr.x0 + 8 * k, pr.y1 - 14 * k, pr.x0 + 8 * k + (pr.w() - 16 * k) * std::clamp(frac, 0.0f, 1.0f), pr.y1 - 8 * k, t.accent);
    }
    if (done) {
        const std::string s = "WATCHED";
        const float tw = measureText(s, 15 * k, FontFace::UIBold);
        fillRound(L, pr.x1 - tw - 24 * k, pr.y0 + 8 * k, pr.x1 - 8 * k, pr.y0 + 34 * k, 6 * k, Color8{0, 0, 0, 190});
        label(L, pr.x1 - tw - 16 * k, pr.y0 + 27 * k, s, 15 * k, FontFace::UIBold, t.accent);
    }
    if (hv) strokeRect(L, pr.x0, pr.y0, pr.x1, pr.y1, 2 * k, withAlpha(t.accent, 230));
}
}  // namespace

float MediaApp::paintShowGrid(PCtx& c) {
    using namespace ui;
    const float k = c.k, pad = c.pad, gap = c.gap;
    const std::string q = lower(q_);
    struct Row {
        int idx = 0, done = 0, total = 0;
        const Info* in = nullptr;
        bool part = false, any = false;
        long long last = 0;
    };
    std::vector<Row> rows;
    for (size_t i = 0; i < shows_.size(); ++i) {
        const Show& s = shows_[i];
        Row r;
        r.idx = static_cast<int>(i);
        r.in = info(s.key);
        r.total = static_cast<int>(s.eps.size());
        for (int v : s.eps) {
            const Progress p = prog(vids_[v].t.path);
            if (p.watched) ++r.done;
            if (p.any && (p.watched || p.pos > 0)) r.any = true;
            r.last = std::max(r.last, p.last);
        }
        r.part = r.any && r.done < r.total;
        if (filterMode_ == 1 && !r.part) continue;
        if (filterMode_ == 2 && r.any) continue;
        if (filterMode_ == 3 && (r.done < r.total || r.total == 0)) continue;
        if (!q.empty() && (lower(s.name) + " " + (r.in ? lower(r.in->genres + " " + r.in->title) : std::string())).find(q) == std::string::npos) continue;
        rows.push_back(r);
    }
    auto year = [](const Row& r) { return r.in ? std::atoi(r.in->year.c_str()) : 0; };
    auto rating = [](const Row& r) { return r.in ? std::atof(r.in->rating.c_str()) : 0.0; };
    if (sortMode_ == 1) std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.last > b.last; });
    else if (sortMode_ == 2) std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) { return year(a) > year(b); });
    else if (sortMode_ == 3) std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) { return rating(a) > rating(b); });
    if (rows.empty()) {
        if (!q.empty() || filterMode_ != 0) c.empty("NO MATCHES", "NO SHOW MATCHES THE SEARCH OR FILTER");
        else if (libScanning_) c.empty("SCANNING", "SHOWS APPEAR HERE AS THEY ARE FOUND");
        else c.empty("NO TV SHOWS YET", "NAME FILES LIKE  SHOW NAME S01E02.MP4  OR PUT THEM IN SHOW\\SEASON 1 FOLDERS");
        return 0;
    }
    const int cols = std::max(2, static_cast<int>((c.view.w() - 2 * pad + gap) / (210 * k + gap)));
    const float tw = (c.view.w() - 2 * pad - gap * (cols - 1)) / cols, ph2 = tw * 1.5f, cellH = ph2 + 76 * k + gap;
    for (size_t n = 0; n < rows.size(); ++n) {
        const Row& r = rows[n];
        const Show& s = shows_[r.idx];
        const int col = static_cast<int>(n) % cols, rw = static_cast<int>(n) / cols;
        const float x0 = c.view.x0 + pad + col * (tw + gap), y0 = c.view.y0 + pad + rw * cellH - scroll_;
        if (!c.onScreen(y0, y0 + cellH)) continue;
        if (!r.in) scrape_.requestShow(s.name);
        const Rect pr{x0, y0, x0 + tw, y0 + ph2};
        const bool hv = c.hot(70, r.idx);
        const float frac = r.total ? static_cast<float>(r.done) / r.total : 0.0f;
        posterTile(c.L, pr, k, c.t, r.in ? webBitmap(r.in->img, 300, 450) : nullptr, s.key, s.name, hv, r.part ? std::max(frac, 0.04f) : 0.0f, r.done == r.total && r.total > 0);
        std::set<int> seasons;
        for (int v : s.eps) seasons.insert(vids_[v].vc.season);
        label(c.L, x0, y0 + ph2 + 28 * k, ellipsize(r.in ? r.in->title : s.name, 22 * k, FontFace::UIBold, tw), 22 * k, FontFace::UIBold, hv ? c.t.accent : c.t.text);
        const std::string sub = std::to_string(seasons.size()) + (seasons.size() == 1 ? " SEASON  /  " : " SEASONS  /  ") + std::to_string(r.total) + " EP";
        label(c.L, x0, y0 + ph2 + 54 * k, ellipsize(sub + (r.in && !r.in->year.empty() ? "  /  " + r.in->year : ""), 18 * k, FontFace::UI, tw), 18 * k, FontFace::UI, c.t.dim);
        c.hit(Rect{x0, y0, x0 + tw, y0 + ph2 + 66 * k}, 70, r.idx);
    }
    return pad * 2 + ((static_cast<int>(rows.size()) + cols - 1) / cols) * cellH;
}

float MediaApp::paintShowPage(PCtx& c) {
    using namespace ui;
    const float k = c.k, pad = c.pad;
    Show& s = shows_[showOpen_];
    const Info* in = info(s.key);
    if (!in) scrape_.requestShow(s.name);
    auto ec = epCache_.find(s.key);
    if (ec == epCache_.end()) ec = epCache_.emplace(s.key, db_.episodes(s.key)).first;
    std::map<std::pair<int, int>, const Episode*> web;
    for (const Episode& e : ec->second) web[{e.season, e.number}] = &e;

    std::set<int> seasons;
    for (int v : s.eps) seasons.insert(vids_[v].vc.season);
    const int ri = resumeIndex(s);
    if (seasonSel_ < 0 || !seasons.count(seasonSel_)) seasonSel_ = ri >= 0 && seasons.count(vids_[ri].vc.season) ? vids_[ri].vc.season : (seasons.empty() ? 0 : *seasons.begin());

    const float x0 = c.view.x0 + pad;
    float y = c.view.y0 + pad - scroll_;
    const float pw = 250 * k, phh = 375 * k;
    const Rect pr{x0, y, x0 + pw, y + phh};
    if (c.onScreen(pr.y0, pr.y1)) posterTile(c.L, pr, k, c.t, in ? webBitmap(in->img, 300, 450) : nullptr, s.key, s.name, false, 0, false);
    const float rx = x0 + pw + 36 * k, rw = c.view.x1 - pad - rx;
    label(c.L, rx, y + 40 * k, ellipsize(in ? in->title : s.name, 40 * k, FontFace::UIBold, rw), 40 * k, FontFace::UIBold, c.t.text);
    int done = 0;
    for (int v : s.eps) if (prog(vids_[v].t.path).watched) ++done;
    std::vector<std::string> meta;
    if (in) {
        meta.push_back(in->year);
        meta.push_back(in->genres);
        if (!in->rating.empty()) meta.push_back("RATING " + in->rating);
        meta.push_back(in->extra);
    } else {
        meta.push_back(scrape_.offline() ? "OFFLINE - SHOW INFO WILL LOAD WHEN YOU ARE ONLINE" : "FETCHING SHOW INFO...");
    }
    label(c.L, rx, y + 76 * k, ellipsize(joinDot(meta), 20 * k, FontFace::UI, rw), 20 * k, FontFace::UI, c.t.accent);
    label(c.L, rx, y + 104 * k, std::to_string(done) + " OF " + std::to_string(s.eps.size()) + " EPISODES WATCHED", 18 * k, FontFace::UI, c.t.dim);
    if (in && !in->summary.empty()) {
        const auto ln = wrapText(in->summary, 21 * k, FontFace::UI, rw, 7);
        for (size_t i = 0; i < ln.size(); ++i) label(c.L, rx, y + (142 + 29 * i) * k, ln[i], 21 * k, FontFace::UI, c.t.text);
    }
    // buttons
    {
        std::string bl = "PLAY S1E1";
        if (ri >= 0) {
            const Progress p = prog(vids_[ri].t.path);
            const VidClass& vc = vids_[ri].vc;
            const std::string se = "S" + std::to_string(vc.season) + "E" + std::to_string(vc.episode);
            bl = (p.any && !p.watched && p.pos > 20 ? "RESUME " : (p.any ? "CONTINUE " : "PLAY ")) + se;
        }
        const float by = y + phh - 52 * k;
        const float bw1 = measureText(bl, 22 * k, FontFace::UIBold) + 56 * k;
        const Rect b1{rx, by, rx + bw1, by + 48 * k}, b2{b1.x1 + 12 * k, by, b1.x1 + 12 * k + 120 * k, by + 48 * k};
        if (c.onScreen(by, by + 48 * k)) {
            button(c.L, b1, bl, c.hot(74, 0) ? State::Hover : State::Normal, true, 22 * k);
            button(c.L, b2, "BACK", c.hot(71, 0) ? State::Hover : State::Normal, false, 22 * k);
            c.hit(b1, 74, 0);
            c.hit(b2, 71, 0);
        }
    }
    y += phh + 30 * k;
    // season chips
    {
        float sx = x0;
        for (int se : seasons) {
            const std::string txt = "SEASON " + std::to_string(se);
            const float w = measureText(txt, 20 * k, FontFace::UIBold) + 36 * k;
            const Rect r{sx, y, sx + w, y + 42 * k};
            if (c.onScreen(r.y0, r.y1)) {
                button(c.L, r, txt, se == seasonSel_ ? State::Selected : (c.hot(72, se) ? State::Hover : State::Normal), se == seasonSel_, 20 * k);
                c.hit(r, 72, se);
            }
            sx += w + 10 * k;
        }
    }
    y += 62 * k;
    // episodes
    const float rowH = 144 * k, tw = 224 * k, th = 126 * k;
    for (int v : s.eps) {
        Vid& vd = vids_[v];
        if (vd.vc.season != seasonSel_) continue;
        const Rect row{x0, y, c.view.x1 - pad, y + rowH - 8 * k};
        if (c.onScreen(row.y0, row.y1)) {
            const Progress p = prog(vd.t.path);
            const bool hv = c.hot(73, v), playing = now_.active && now_.path == vd.t.path;
            if (hv || playing) fillRound(c.L, row.x0, row.y0, row.x1, row.y1, 8 * k, withAlpha(c.t.accent, playing ? 40 : 20));
            const auto wf = web.find({vd.vc.season, vd.vc.episode});
            const Episode* ep = wf == web.end() ? nullptr : wf->second;
            const Rect tr{row.x0 + 8 * k, row.y0 + 6 * k, row.x0 + 8 * k + tw, row.y0 + 6 * k + th};
            const Bitmap* im = ep ? webBitmap(ep->img, 384, 216) : nullptr;
            if (!im) im = thumbFor(vd);
            double secs = 0;
            auto tg = tagCache_.find(vd.t.path);
            if (tg != tagCache_.end()) secs = tg->second.seconds;
            else if (epTagAsked_.insert(vd.t.path).second) meta_.requestTags(vd.t.path);
            if (secs > 0 && db_.duration(vd.t.path) <= 0) db_.setDuration(vd.t.path, secs);
            if (secs <= 0) secs = p.dur > 0 ? p.dur : db_.duration(vd.t.path);
            if (secs <= 0 && ep && ep->runtime > 0) secs = ep->runtime * 60.0;
            const float frac = p.dur > 0 ? static_cast<float>(p.pos / p.dur) : (secs > 0 ? static_cast<float>(p.pos / secs) : 0.0f);
            posterTile(c.L, tr, k, c.t, im, vd.t.path, vd.t.title, hv, p.pos > 0 ? std::max(frac, 0.03f) : 0.0f, false);
            if (hv || playing) {
                const float cx = (tr.x0 + tr.x1) * 0.5f, cy = (tr.y0 + tr.y1) * 0.5f;
                dot(c.L, cx, cy, 24 * k, withAlpha(c.t.accent, 225));
                triangle(c.L, cx, cy, 22 * k, true, Color8{8, 13, 20, 255});
            }
            const float tx = tr.x1 + 24 * k, tw2 = row.x1 - tx - 200 * k;
            const std::string nm = ep && !ep->name.empty() ? ep->name : vd.t.title;
            label(c.L, tx, row.y0 + 36 * k, ellipsize(std::to_string(vd.vc.episode) + ".  " + nm, 25 * k, FontFace::UIBold, tw2), 25 * k, FontFace::UIBold, hv || playing ? c.t.accent : c.t.text);
            std::vector<std::string> sm;
            if (secs > 0) sm.push_back(durText(secs));
            if (ep && !ep->airdate.empty()) sm.push_back(ep->airdate);
            if (p.watched) sm.push_back("WATCHED");
            else if (p.pos > 20 && secs > 0) sm.push_back(durText(std::max(0.0, secs - p.pos)) + " LEFT");
            label(c.L, tx, row.y0 + 64 * k, joinDot(sm), 18 * k, FontFace::UI, p.watched ? c.t.success : c.t.dim);
            if (ep && !ep->summary.empty()) {
                const auto ln = wrapText(ep->summary, 18 * k, FontFace::UI, tw2, 3);
                for (size_t i = 0; i < ln.size(); ++i) label(c.L, tx, row.y0 + (92 + 24 * i) * k, ln[i], 18 * k, FontFace::UI, c.t.dim);
            }
            c.hit(row, 73, v);
            const Rect wb{row.x1 - 180 * k, row.y0 + (rowH - 8 * k - 40 * k) * 0.5f, row.x1 - 10 * k, row.y0 + (rowH - 8 * k + 40 * k) * 0.5f};
            button(c.L, wb, p.watched ? "UNWATCH" : "MARK WATCHED", c.hot(75, v) ? State::Hover : State::Normal, false, 16 * k);
            c.hit(wb, 75, v);
        }
        y += rowH;
    }
    return y + scroll_ - c.view.y0 + pad;
}

float MediaApp::paintMovieGrid(PCtx& c) {
    using namespace ui;
    const float k = c.k, pad = c.pad, gap = c.gap;
    const std::string q = lower(q_);
    struct Row {
        int pos = 0;
        const Info* in = nullptr;
        Progress p;
        int year = 0;
    };
    std::vector<Row> rows;
    for (size_t i = 0; i < movies_.size(); ++i) {
        const Vid& v = vids_[movies_[i]];
        Row r;
        r.pos = static_cast<int>(i);
        r.in = info(MediaScrape::movieKey(v.vc.name, v.vc.year));
        r.p = prog(v.t.path);
        r.year = r.in && !r.in->year.empty() ? std::atoi(r.in->year.c_str()) : v.vc.year;
        if (!filterOk(r.p)) continue;
        if (!q.empty() && (lower(v.vc.name) + " " + (r.in ? lower(r.in->genres + " " + r.in->title + " " + r.in->extra) : std::string())).find(q) == std::string::npos) continue;
        rows.push_back(r);
    }
    if (sortMode_ == 1) std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.p.last > b.p.last; });
    else if (sortMode_ == 2) std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.year > b.year; });
    else if (sortMode_ == 3) std::reverse(rows.begin(), rows.end());
    if (rows.empty()) {
        if (!q.empty() || filterMode_ != 0) c.empty("NO MATCHES", "NO MOVIE MATCHES THE SEARCH OR FILTER");
        else if (libScanning_) c.empty("SCANNING", "MOVIES APPEAR HERE AS THEY ARE FOUND");
        else c.empty("NO MOVIES YET", "NAME FILES LIKE  MOVIE NAME (1999).MP4  OR KEEP THEM IN A FOLDER CALLED MOVIES");
        return 0;
    }
    const int cols = std::max(2, static_cast<int>((c.view.w() - 2 * pad + gap) / (210 * k + gap)));
    const float tw = (c.view.w() - 2 * pad - gap * (cols - 1)) / cols, ph2 = tw * 1.5f, cellH = ph2 + 76 * k + gap;
    for (size_t n = 0; n < rows.size(); ++n) {
        const Row& r = rows[n];
        const Vid& v = vids_[movies_[r.pos]];
        const int col = static_cast<int>(n) % cols, rw = static_cast<int>(n) / cols;
        const float x0 = c.view.x0 + pad + col * (tw + gap), y0 = c.view.y0 + pad + rw * cellH - scroll_;
        if (!c.onScreen(y0, y0 + cellH)) continue;
        if (!r.in) scrape_.requestMovie(v.vc.name, v.vc.year);
        const Rect pr{x0, y0, x0 + tw, y0 + ph2};
        const bool hv = c.hot(80, r.pos);
        const float frac = r.p.dur > 0 ? static_cast<float>(r.p.pos / r.p.dur) : 0.0f;
        posterTile(c.L, pr, k, c.t, r.in ? webBitmap(r.in->img, 300, 450) : nullptr, v.vc.name, v.vc.name, hv, r.p.pos > 0 ? std::max(frac, 0.04f) : 0.0f, r.p.watched);
        label(c.L, x0, y0 + ph2 + 28 * k, ellipsize(r.in ? r.in->title : v.vc.name, 22 * k, FontFace::UIBold, tw), 22 * k, FontFace::UIBold, hv ? c.t.accent : c.t.text);
        label(c.L, x0, y0 + ph2 + 54 * k, ellipsize(joinDot({r.year ? std::to_string(r.year) : std::string(), r.in ? r.in->genres : std::string()}), 18 * k, FontFace::UI, tw), 18 * k, FontFace::UI, c.t.dim);
        c.hit(Rect{x0, y0, x0 + tw, y0 + ph2 + 66 * k}, 80, r.pos);
    }
    return pad * 2 + ((static_cast<int>(rows.size()) + cols - 1) / cols) * cellH;
}

float MediaApp::paintMoviePage(PCtx& c) {
    using namespace ui;
    const float k = c.k, pad = c.pad;
    const int vi = movies_[movieOpen_];
    Vid& v = vids_[vi];
    const Info* in = info(MediaScrape::movieKey(v.vc.name, v.vc.year));
    if (!in) scrape_.requestMovie(v.vc.name, v.vc.year);
    const Progress p = prog(v.t.path);
    const float x0 = c.view.x0 + pad, y = c.view.y0 + pad - scroll_;
    const float pw = 300 * k, phh = 450 * k;
    const Rect pr{x0, y, x0 + pw, y + phh};
    posterTile(c.L, pr, k, c.t, in ? webBitmap(in->img, 300, 450) : nullptr, v.vc.name, v.vc.name, false, 0, false);
    const float rx = x0 + pw + 40 * k, rw = c.view.x1 - pad - rx;
    label(c.L, rx, y + 44 * k, ellipsize(in ? in->title : v.vc.name, 44 * k, FontFace::UIBold, rw), 44 * k, FontFace::UIBold, c.t.text);
    std::vector<std::string> meta;
    if (in) {
        meta.push_back(in->year.empty() && v.vc.year ? std::to_string(v.vc.year) : in->year);
        meta.push_back(in->genres);
        meta.push_back(in->extra2);
    } else {
        meta.push_back(scrape_.offline() ? "OFFLINE - MOVIE INFO WILL LOAD WHEN YOU ARE ONLINE" : "FETCHING MOVIE INFO...");
    }
    label(c.L, rx, y + 82 * k, ellipsize(joinDot(meta), 21 * k, FontFace::UI, rw), 21 * k, FontFace::UI, c.t.accent);
    float ly = y + 114 * k;
    std::string intro, plot;
    if (in) {
        const size_t br = in->summary.find("\n\n");
        intro = in->summary.substr(0, br);
        if (br != std::string::npos) plot = in->summary.substr(br + 2);
    }
    if (in && !in->extra.empty()) {  // credits: "LABEL<TAB>value" lines
        size_t pos = 0;
        int shown = 0;
        while (pos < in->extra.size() && shown < 4) {
            size_t nl = in->extra.find('\n', pos);
            if (nl == std::string::npos) nl = in->extra.size();
            const std::string line = in->extra.substr(pos, nl - pos);
            pos = nl + 1;
            const size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            const std::string lab = line.substr(0, tab);
            if (lab == "COUNTRY" || lab == "BUDGET" || lab == "BOX OFFICE" || lab == "MUSIC BY" || lab == "STUDIO") continue;
            label(c.L, rx, ly + 18 * k, lab, 16 * k, FontFace::UIBold, c.t.muted);
            const float lw = 150 * k;
            const auto vl = wrapText(line.substr(tab + 1), 19 * k, FontFace::UI, rw - lw, 2);
            for (size_t i = 0; i < vl.size(); ++i) label(c.L, rx + lw, ly + (18 + 24 * i) * k, vl[i], 19 * k, FontFace::UI, c.t.text);
            ly += (vl.size() > 1 ? 52 : 28) * k;
            ++shown;
        }
        ly += 6 * k;
    }
    if (!intro.empty()) {
        const int maxLines = std::max(2, static_cast<int>((y + phh - 70 * k - ly) / (28 * k)));
        const auto ln = wrapText(intro, 21 * k, FontFace::UI, rw, maxLines);
        for (size_t i = 0; i < ln.size(); ++i) label(c.L, rx, ly + (22 + 28 * i) * k, ln[i], 21 * k, FontFace::UI, c.t.text);
    }    const float by = y + phh - 52 * k;
    std::string bl = "PLAY";
    if (p.any && !p.watched && p.pos > 20) bl = "RESUME  " + clock(p.pos);
    else if (p.watched) bl = "WATCH AGAIN";
    const float bw1 = measureText(bl, 22 * k, FontFace::UIBold) + 56 * k;
    const Rect b1{rx, by, rx + bw1, by + 48 * k}, b2{b1.x1 + 12 * k, by, b1.x1 + 12 * k + 220 * k, by + 48 * k}, b3{b2.x1 + 12 * k, by, b2.x1 + 12 * k + 120 * k, by + 48 * k};
    button(c.L, b1, bl, c.hot(82, vi) ? State::Hover : State::Normal, true, 22 * k);
    button(c.L, b2, p.watched ? "MARK UNWATCHED" : "MARK WATCHED", c.hot(75, vi) ? State::Hover : State::Normal, false, 18 * k);
    button(c.L, b3, "BACK", c.hot(81, 0) ? State::Hover : State::Normal, false, 22 * k);
    c.hit(b1, 82, vi);
    c.hit(b2, 75, vi);
    c.hit(b3, 81, 0);
    if (p.any && !p.watched && p.dur > 0 && p.pos > 0) bar(c.L, Rect{rx, by - 22 * k, rx + 420 * k, by - 12 * k}, static_cast<float>(p.pos / p.dur), c.t.accent);
    label(c.L, rx, y + phh + 40 * k, ellipsize(v.t.path, 17 * k, FontFace::UI, rw), 17 * k, FontFace::UI, c.t.muted);
    return phh + 2 * pad + 60 * k;
}

float MediaApp::paintArtistGrid(PCtx& c) {
    using namespace ui;
    const float k = c.k, pad = c.pad, gap = c.gap;
    const std::string q = lower(q_);
    std::vector<int> vis;
    for (size_t i = 0; i < artists_.size(); ++i) {
        const Artist& a = artists_[i];
        if (!q.empty()) {
            bool hit = lower(a.name).find(q) != std::string::npos;
            for (size_t j = 0; j < a.albums.size() && !hit; ++j) hit = lower(albums_[a.albums[j]].title).find(q) != std::string::npos;
            if (!hit) continue;
        }
        vis.push_back(static_cast<int>(i));
    }
    if (sortMode_ == 1) std::reverse(vis.begin(), vis.end());
    else if (sortMode_ == 2) std::stable_sort(vis.begin(), vis.end(), [&](int a, int b) { return artists_[a].albums.size() > artists_[b].albums.size(); });
    else if (sortMode_ == 3) std::stable_sort(vis.begin(), vis.end(), [&](int a, int b) { return artists_[a].tracks > artists_[b].tracks; });
    if (vis.empty()) {
        if (!q.empty()) c.empty("NO MATCHES", "NOTHING IN YOUR MUSIC MATCHES THE SEARCH");
        else if (libScanning_) c.empty("SCANNING YOUR MUSIC", "ARTISTS APPEAR HERE AS THEY ARE FOUND");
        else c.empty("NO MUSIC YET", "ADD YOUR MUSIC AND VIDEO FOLDERS, OR DROP A FOLDER ONTO THE WINDOW");
        return 0;
    }
    const int cols = std::max(2, static_cast<int>((c.view.w() - 2 * pad + gap) / (220 * k + gap)));
    const float tw = (c.view.w() - 2 * pad - gap * (cols - 1)) / cols, cellH = tw + 76 * k + gap;
    for (size_t n = 0; n < vis.size(); ++n) {
        const int ai = vis[n];
        Artist& a = artists_[ai];
        const int col = static_cast<int>(n) % cols, rw = static_cast<int>(n) / cols;
        const float x0 = c.view.x0 + pad + col * (tw + gap), y0 = c.view.y0 + pad + rw * cellH - scroll_;
        if (!c.onScreen(y0, y0 + cellH)) continue;
        const bool real = a.name != "Unknown Artist" && a.name != "Various Artists";
        const Info* in = real ? info(MediaScrape::artistKey(a.name)) : nullptr;
        if (real && !in) scrape_.requestArtist(a.name);
        const Rect pr{x0, y0, x0 + tw, y0 + tw};
        const bool hv = c.hot(90, ai);
        const Bitmap* web = in ? webBitmap(in->img, 400, 400) : nullptr;
        if (web) {
            posterTile(c.L, pr, k, c.t, web, a.key, a.name, hv, 0, false);
        } else {
            // composite of the album covers (up to four) until a picture is found
            std::vector<const Bitmap*> covers;
            for (size_t j = 0; j < a.albums.size() && covers.size() < 4; ++j)
                if (const Bitmap* b = artFor(albums_[a.albums[j]])) covers.push_back(b);
            if (covers.empty()) posterTile(c.L, pr, k, c.t, nullptr, a.key, a.name, hv, 0, false);
            else if (covers.size() == 1 || a.albums.size() == 1) posterTile(c.L, pr, k, c.t, covers[0], a.key, a.name, hv, 0, false);
            else {
                const float hw = tw * 0.5f;
                for (size_t j = 0; j < 4; ++j) {
                    const float qx = x0 + (j % 2) * hw, qy = y0 + (j / 2) * hw;
                    drawImage(c.L, *covers[j % covers.size()], Rect{qx, qy, qx + hw, qy + hw}, 0);
                }
                if (hv) fillRound(c.L, pr.x0, pr.y0, pr.x1, pr.y1, 8 * k, Color8{0, 0, 0, 80});
                if (hv) strokeRect(c.L, pr.x0, pr.y0, pr.x1, pr.y1, 2 * k, withAlpha(c.t.accent, 230));
            }
        }
        label(c.L, x0, y0 + tw + 28 * k, ellipsize(a.name, 22 * k, FontFace::UIBold, tw), 22 * k, FontFace::UIBold, hv ? c.t.accent : c.t.text);
        label(c.L, x0, y0 + tw + 54 * k, std::to_string(a.albums.size()) + (a.albums.size() == 1 ? " ALBUM  /  " : " ALBUMS  /  ") + std::to_string(a.tracks) + " TRACKS", 18 * k, FontFace::UI, c.t.dim);
        c.hit(Rect{x0, y0, x0 + tw, y0 + tw + 66 * k}, 90, ai);
    }
    return pad * 2 + ((static_cast<int>(vis.size()) + cols - 1) / cols) * cellH;
}

float MediaApp::paintArtistPage(PCtx& c) {
    using namespace ui;
    const float k = c.k, pad = c.pad, gap = c.gap;
    Artist& a = artists_[artistOpen_];
    const bool real = a.name != "Unknown Artist" && a.name != "Various Artists";
    const Info* in = real ? info(MediaScrape::artistKey(a.name)) : nullptr;
    if (real && !in) scrape_.requestArtist(a.name);
    const float lw = 340 * k;
    const Rect pic{c.view.x0 + pad, c.view.y0 + pad, c.view.x0 + pad + lw, c.view.y0 + pad + lw};
    const Bitmap* web = in ? webBitmap(in->img, 400, 400) : nullptr;
    const Bitmap* art = web;
    if (!art && !a.albums.empty()) art = artFor(albums_[a.albums[0]]);
    posterTile(c.L, pic, k, c.t, art, a.key, a.name, false, 0, false);
    float ly = pic.y1 + 38 * k;
    for (const std::string& ln : wrapText(a.name, 34 * k, FontFace::UIBold, lw, 2)) {
        label(c.L, pic.x0, ly, ln, 34 * k, FontFace::UIBold, c.t.text);
        ly += 40 * k;
    }
    const std::string sub = joinDot({std::to_string(a.albums.size()) + (a.albums.size() == 1 ? " ALBUM" : " ALBUMS"), std::to_string(a.tracks) + " TRACKS", in ? in->extra : std::string()});
    label(c.L, pic.x0, ly, ellipsize(sub, 19 * k, FontFace::UI, lw), 19 * k, FontFace::UI, c.t.accent);
    ly += 20 * k;
    const float bw = (lw - 10 * k) / 2;
    const Rect b1{pic.x0, ly, pic.x0 + bw, ly + 44 * k}, b2{b1.x1 + 10 * k, ly, pic.x1, ly + 44 * k};
    button(c.L, b1, "SHUFFLE", c.hot(92, 0) ? State::Hover : State::Normal, true, 20 * k);
    button(c.L, b2, "BACK", c.hot(91, 0) ? State::Hover : State::Normal, false, 20 * k);
    c.hit(b1, 92, 0);
    c.hit(b2, 91, 0);
    ly += 76 * k;
    if (in && !in->summary.empty()) {
        const int maxLines = std::max(0, static_cast<int>((c.view.y1 - ly) / (25 * k)));
        if (maxLines > 0) {
            const auto ln = wrapText(in->summary, 18 * k, FontFace::UI, lw, maxLines);
            for (size_t i = 0; i < ln.size(); ++i) label(c.L, pic.x0, ly + 25 * k * i, ln[i], 18 * k, FontFace::UI, c.t.dim);
        }
    } else if (real && !in) {
        label(c.L, pic.x0, ly, scrape_.offline() ? "OFFLINE - ARTIST INFO LOADS WHEN ONLINE" : "FETCHING ARTIST INFO...", 18 * k, FontFace::UI, c.t.muted);
    }

    const float gx0 = c.view.x0 + lw + 50 * k, gx1 = c.view.x1 - pad;
    const int cols = std::max(2, static_cast<int>((gx1 - gx0 + gap) / (210 * k + gap)));
    const float tw = (gx1 - gx0 - gap * (cols - 1)) / cols, cellH = tw + 76 * k + gap;
    std::vector<int> order = a.albums;
    std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return albums_[x].year < albums_[y].year; });
    for (size_t n = 0; n < order.size(); ++n) {
        const int ai = order[n];
        Album& al = albums_[ai];
        const int col = static_cast<int>(n) % cols, rw = static_cast<int>(n) / cols;
        const float x0 = gx0 + col * (tw + gap), y0 = c.view.y0 + pad + rw * cellH - scroll_;
        if (!c.onScreen(y0, y0 + cellH)) continue;
        const Rect cv{x0, y0, x0 + tw, y0 + tw};
        const bool hv = c.hot(20, ai);
        posterTile(c.L, cv, k, c.t, artFor(al), al.key, al.title, hv, 0, false);
        label(c.L, x0, y0 + tw + 28 * k, ellipsize(al.title, 21 * k, FontFace::UIBold, tw), 21 * k, FontFace::UIBold, hv ? c.t.accent : c.t.text);
        label(c.L, x0, y0 + tw + 54 * k, joinDot({al.year ? std::to_string(al.year) : std::string(), std::to_string(al.items.size()) + " TRACKS"}), 18 * k, FontFace::UI, c.t.dim);
        c.hit(Rect{x0, y0, x0 + tw, y0 + tw + 66 * k}, 20, ai);
    }
    return pad * 2 + ((static_cast<int>(order.size()) + cols - 1) / cols) * cellH;
}

// Lyrics for the selected track, fetched from lrclib.net and kept in the database.
float MediaApp::drawLyrics(PCtx& c, float x0, float x1) {
    using namespace ui;
    const float k = c.k, pad = c.pad;
    const Item& it = items_[lyricsItem_];
    const std::string ar = it.m.artist.empty() ? (albumOpen_ >= 0 ? albums_[albumOpen_].artist : std::string()) : it.m.artist;
    const Info* in = info(MediaScrape::lyricsKey(ar, it.title));
    if (!in) scrape_.requestLyrics(ar, it.title, it.m.seconds);
    float y = c.view.y0 + pad - scroll_;
    const Rect back{x1 - 200 * k, c.view.y0 + pad, x1, c.view.y0 + pad + 44 * k};
    label(c.L, x0, y + 34 * k, ellipsize(it.title, 30 * k, FontFace::UIBold, x1 - x0 - 230 * k), 30 * k, FontFace::UIBold, c.t.accent);
    y += 70 * k;
    if (!in) {
        label(c.L, x0, y + 24 * k, scrape_.offline() ? "OFFLINE - LYRICS LOAD WHEN YOU ARE ONLINE" : "FETCHING LYRICS...", 22 * k, FontFace::UI, c.t.dim);
        y += 40 * k;
    } else if (!in->found) {
        label(c.L, x0, y + 24 * k, "NO LYRICS FOUND FOR THIS TRACK", 22 * k, FontFace::UI, c.t.dim);
        y += 40 * k;
    } else {
        size_t pos = 0;
        const std::string& s = in->summary;
        while (pos <= s.size()) {
            size_t e = s.find('\n', pos);
            if (e == std::string::npos) e = s.size();
            std::string line = s.substr(pos, e - pos);
            pos = e + 1;
            if (line.empty()) { y += 18 * k; continue; }
            for (const std::string& w : wrapText(line, 24 * k, FontFace::UI, x1 - x0, 3)) {
                if (c.onScreen(y, y + 32 * k)) label(c.L, x0, y + 24 * k, w, 24 * k, FontFace::UI, c.t.text);
                y += 32 * k;
            }
        }
    }
    button(c.L, back, "TRACKS", c.hot(94, 0) ? State::Hover : State::Normal, false, 20 * k);
    c.hit(back, 94, 0);
    return y + scroll_ - c.view.y0 + pad;
}

}  // namespace sf
