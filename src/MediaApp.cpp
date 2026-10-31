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

MediaApp::MediaApp(Renderer& renderer, MediaHost host) : PanelApp(renderer, 1600, 900, 2.2f), host_(std::move(host)) {
    title_ = "MEDIA";
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
    tab_ = std::clamp(t, 0, 2);
    theatre_ = false;
    manage_ = false;
    albumOpen_ = -1;
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
    if (albumOpen_ >= 0) {
        albumOpen_ = -1;
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
            vids_.push_back(std::move(v));
        } else {
            Item it;
            it.t = t;
            items_.push_back(std::move(it));
            if (tagAsked_.insert(t.path).second) {
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
    MetaResult r;
    int n = 0;
    while (n < 40 && meta_.poll(r)) {
        ++n;
        if (r.kind == MetaResult::Tags) {
            tagCache_[r.key] = r.meta;
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
}

bool MediaApp::albumMatches(const Album& a) const {
    if (q_.empty()) return true;
    const std::string q = lower(q_);
    return lower(a.title).find(q) != std::string::npos || lower(a.artist).find(q) != std::string::npos;
}

const Bitmap* MediaApp::artFor(const Album& a) {
    auto f = artCache_.find(a.key);
    if (f != artCache_.end()) return &f->second;
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
    if (!searchFocus_) return;
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
        case 4: searchFocus_ = true; break;
        case 10: if (host_.toggle) host_.toggle(); break;
        case 11: if (host_.prev) host_.prev(); break;
        case 12: if (host_.next) host_.next(); break;
        case 13: if (host_.toggleShuffle) host_.toggleShuffle(); break;
        case 14: if (host_.cycleRepeat) host_.cycleRepeat(); break;
        case 15: if (host_.seek && now_.dur > 0) host_.seek(frac(15) * now_.dur); break;
        case 16: if (host_.setVolume) host_.setVolume(frac(16)); break;
        case 20: albumOpen_ = arg; scroll_ = scrollTarget_ = 0; break;
        case 21: playAlbum(arg, 0, false); break;
        case 22: albumOpen_ = -1; scroll_ = scrollTarget_ = 0; break;
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
    if (tab_ == 0) sub = std::to_string(nAlb) + " ALBUMS  /  " + std::to_string(items_.size()) + " TRACKS";
    else if (tab_ == 1) sub = std::to_string(vids_.size()) + " VIDEOS";
    else sub = std::to_string(stations_.size()) + " STATIONS";
    if (libScanning_) sub = "SCANNING  " + std::to_string(host_.scanned ? host_.scanned() : 0) + " FILES  /  " + sub;
    const Rect c = panel(bm, "MEDIA", sub);

    // toolbar
    const float ty = c.y0 + 6 * k, th = 44 * k;
    float x = c.x0 + 14 * k;
    const char* tabs[3] = {"MUSIC", "VIDEO", "RADIO"};
    for (int i = 0; i < 3; ++i) {
        Rect r{x, ty, x + 150 * k, ty + th};
        State s = tab_ == i ? State::Selected : (hot(1 + i, 0) ? State::Hover : State::Normal);
        button(bm, r, tabs[i], s, tab_ == i, 24 * k);
        addHit(r, 1 + i);
        x += 158 * k;
    }
    if (!theatre_) {
        const bool busy = host_.pickBusy && host_.pickBusy();
        Rect r{x + 8 * k, ty, x + 8 * k + 210 * k, ty + th};
        button(bm, r, busy ? "CHOOSING..." : "ADD FOLDERS", hot(52, 0) ? State::Hover : State::Normal, false, 20 * k);
        addHit(r, 52);
        Rect m{r.x1 + 8 * k, ty, r.x1 + 8 * k + 140 * k, ty + th};
        button(bm, m, manage_ ? "DONE" : "MANAGE", manage_ ? State::Selected : (hot(51, 0) ? State::Hover : State::Normal), manage_, 20 * k);
        addHit(m, 51);
    }
    const bool searchable = tab_ != 2;
    if (searchable && !theatre_) {
        const Rect sr{c.x1 - 14 * k - 400 * k, ty, c.x1 - 14 * k, ty + th};
        card(bm, sr, searchFocus_ ? State::Selected : (hot(4, 0) ? State::Hover : State::Normal));
        addHit(sr, 4);
        const bool caret = searchFocus_ && (SDL_GetTicks() / 500) % 2 == 0;
        if (q_.empty() && !searchFocus_) label(bm, sr.x0 + 16 * k, sr.y0 + 30 * k, "SEARCH", 22 * k, FontFace::UI, t.muted);
        else label(bm, sr.x0 + 16 * k, sr.y0 + 30 * k, ellipsize(q_, 24 * k, FontFace::UI, sr.w() - 44 * k) + (caret ? "|" : ""), 24 * k, FontFace::UI, t.text);
    }

    // bar geometry first, so the scrolling region can end above it
    const float barH = 140 * k;
    const Rect barR{c.x0, c.y1 - barH, c.x1, c.y1};
    const Rect view{c.x0, ty + th + 12 * k, c.x1, barR.y0 - 12 * k};

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
        label(L, cover.x0, ly, meta, 20 * k, FontFace::UI, t.dim);
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
        for (size_t i = 0; i < al.items.size(); ++i) {
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
                label(L, r.x0 + 70 * k, base, ellipsize(it.title, 24 * k, FontFace::UI, r.w() - 70 * k - dw - 40 * k), 24 * k, FontFace::UI, playing ? t.accent : t.text);
                labelRight(L, r.x1 - 16 * k, base, dur, 20 * k, FontFace::UI, t.dim);
                addHit(r, 25, static_cast<int>(i), &view);
            }
            y += rowH;
        }
        contentH = y + scroll_ - view.y0 + pad;
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
            if (q.empty() || lower(vids_[i].t.title).find(q) != std::string::npos || lower(vids_[i].t.folder).find(q) != std::string::npos)
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

}  // namespace sf
