#include "Thumbnails.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>#include <cstring>
#include <filesystem>
#include <fstream>

#include "TextRender.h"
#include "stb_image.h"
#include "stb_image_write.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <objbase.h>
#endif

namespace sf {

namespace {

namespace fs = std::filesystem;

std::string lowerExt(const std::string& path) {
    std::string e = fs::u8path(path).extension().u8string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

bool inList(const std::string& e, std::initializer_list<const char*> list) {
    for (const char* s : list)
        if (e == s) return true;
    return false;
}

bool readPrefix(const std::string& path, size_t maxBytes, std::vector<unsigned char>& out, uintmax_t* fullSize = nullptr) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    auto size = static_cast<uintmax_t>(f.tellg());
    if (fullSize) *fullSize = size;
    f.seekg(0);
    out.resize(static_cast<size_t>(std::min<uintmax_t>(size, maxBytes)));
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<size_t>(f.gcount()));
    return true;
}

// ---- drawing helpers (all on straight-alpha RGBA) -----------------------------------------------

void blendPixel(Bitmap& bm, int x, int y, Color8 c, float coverage) {
    if (x < 0 || y < 0 || x >= bm.w || y >= bm.h) return;
    float a = coverage * (c.a / 255.0f);
    if (a <= 0.0f) return;
    uint8_t* d = &bm.rgba[(static_cast<size_t>(y) * bm.w + x) * 4];
    d[0] = static_cast<uint8_t>(c.r * a + d[0] * (1.0f - a) + 0.5f);
    d[1] = static_cast<uint8_t>(c.g * a + d[1] * (1.0f - a) + 0.5f);
    d[2] = static_cast<uint8_t>(c.b * a + d[2] * (1.0f - a) + 0.5f);
    d[3] = 255;
}

void fillRoundRect(Bitmap& bm, float x, float y, float w, float h, float r, Color8 c) {
    int x0 = static_cast<int>(std::floor(x)) - 1, x1 = static_cast<int>(std::ceil(x + w)) + 1;
    int y0 = static_cast<int>(std::floor(y)) - 1, y1 = static_cast<int>(std::ceil(y + h)) + 1;
    float cx = x + w * 0.5f, cy = y + h * 0.5f;
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) {
            float qx = std::fabs(px + 0.5f - cx) - (w * 0.5f - r);
            float qy = std::fabs(py + 0.5f - cy) - (h * 0.5f - r);
            float d = std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) + std::max(qy, 0.0f) * std::max(qy, 0.0f)) +
                      std::min(std::max(qx, qy), 0.0f) - r;
            float cov = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (cov > 0) blendPixel(bm, px, py, c, cov);
        }
}

void fillTriangle(Bitmap& bm, std::array<float, 6> t, Color8 c) {
    auto edge = [](float ax, float ay, float bx, float by, float px, float py) {
        return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
    };
    int x0 = static_cast<int>(std::floor(std::min({t[0], t[2], t[4]}))), x1 = static_cast<int>(std::ceil(std::max({t[0], t[2], t[4]})));
    int y0 = static_cast<int>(std::floor(std::min({t[1], t[3], t[5]}))), y1 = static_cast<int>(std::ceil(std::max({t[1], t[3], t[5]})));
    for (int py = y0; py <= y1; ++py)
        for (int px = x0; px <= x1; ++px) {
            int hits = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    float fx = px + (sx + 0.5f) / 4.0f, fy = py + (sy + 0.5f) / 4.0f;
                    float e0 = edge(t[0], t[1], t[2], t[3], fx, fy), e1 = edge(t[2], t[3], t[4], t[5], fx, fy),
                          e2 = edge(t[4], t[5], t[0], t[1], fx, fy);
                    if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)) ++hits;
                }
            if (hits) blendPixel(bm, px, py, c, hits / 16.0f);
        }
}

void fillBackground(Bitmap& bm, Color8 top, Color8 bottom) {
    for (int y = 0; y < bm.h; ++y) {
        float t = static_cast<float>(y) / (bm.h - 1);
        for (int x = 0; x < bm.w; ++x) {
            uint8_t* d = &bm.rgba[(static_cast<size_t>(y) * bm.w + x) * 4];
            d[0] = static_cast<uint8_t>(top.r + (bottom.r - top.r) * t);
            d[1] = static_cast<uint8_t>(top.g + (bottom.g - top.g) * t);
            d[2] = static_cast<uint8_t>(top.b + (bottom.b - top.b) * t);
            d[3] = 255;
        }
    }
}

// Scales src to cover dst (centre crop) with area averaging when shrinking, bilinear when enlarging.
void blitCover(Bitmap& dst, const uint8_t* src, int sw, int sh, Color8 bg) {
    float dAspect = static_cast<float>(dst.w) / dst.h, sAspect = static_cast<float>(sw) / sh;
    float cw = sw, ch = sh;
    if (sAspect > dAspect) cw = sh * dAspect; else ch = sw / dAspect;
    float cx = (sw - cw) * 0.5f, cy = (sh - ch) * 0.5f;
    float stepX = cw / dst.w, stepY = ch / dst.h;

    auto fetch = [&](int x, int y, float* rgb) {
        x = std::clamp(x, 0, sw - 1);
        y = std::clamp(y, 0, sh - 1);
        const uint8_t* p = src + (static_cast<size_t>(y) * sw + x) * 4;
        float a = p[3] / 255.0f;
        rgb[0] = p[0] * a + bg.r * (1 - a);
        rgb[1] = p[1] * a + bg.g * (1 - a);
        rgb[2] = p[2] * a + bg.b * (1 - a);
    };

    for (int y = 0; y < dst.h; ++y)
        for (int x = 0; x < dst.w; ++x) {
            float rgb[3] = {0, 0, 0};
            float fx0 = cx + x * stepX, fy0 = cy + y * stepY;
            if (stepX > 1.0f || stepY > 1.0f) {
                int ix0 = static_cast<int>(fx0), iy0 = static_cast<int>(fy0);
                int ix1 = std::max(ix0 + 1, static_cast<int>(std::ceil(fx0 + stepX)));
                int iy1 = std::max(iy0 + 1, static_cast<int>(std::ceil(fy0 + stepY)));
                float n = 0;
                for (int sy = iy0; sy < iy1; ++sy)
                    for (int sx = ix0; sx < ix1; ++sx) {
                        float t[3];
                        fetch(sx, sy, t);
                        rgb[0] += t[0]; rgb[1] += t[1]; rgb[2] += t[2];
                        n += 1;
                    }
                rgb[0] /= n; rgb[1] /= n; rgb[2] /= n;
            } else {
                float sx = fx0 + stepX * 0.5f - 0.5f, sy = fy0 + stepY * 0.5f - 0.5f;
                int ix = static_cast<int>(std::floor(sx)), iy = static_cast<int>(std::floor(sy));
                float tx = sx - ix, ty = sy - iy;
                float a[3], b[3], c[3], d[3];
                fetch(ix, iy, a); fetch(ix + 1, iy, b); fetch(ix, iy + 1, c); fetch(ix + 1, iy + 1, d);
                for (int k = 0; k < 3; ++k)
                    rgb[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
            }
            uint8_t* o = &dst.rgba[(static_cast<size_t>(y) * dst.w + x) * 4];
            o[0] = static_cast<uint8_t>(std::clamp(rgb[0] + 0.5f, 0.0f, 255.0f));
            o[1] = static_cast<uint8_t>(std::clamp(rgb[1] + 0.5f, 0.0f, 255.0f));
            o[2] = static_cast<uint8_t>(std::clamp(rgb[2] + 0.5f, 0.0f, 255.0f));
            o[3] = 255;
        }
}

void drawScrimAndName(Bitmap& bm, const std::string& name) {
    const int strip = 84;
    for (int y = bm.h - strip; y < bm.h; ++y) {
        float t = static_cast<float>(y - (bm.h - strip)) / strip;
        Color8 black{0, 0, 0, static_cast<uint8_t>(210.0f * t * t + 40.0f * t)};
        for (int x = 0; x < bm.w; ++x) blendPixel(bm, x, y, black, 1.0f);
    }
    const float px = 23.0f;
    std::string text = ellipsize(name, px, FontFace::UI, bm.w - 32.0f);
    drawText(bm, 16.0f, bm.h - 18.0f, text, px, FontFace::UI, Color8{255, 255, 255, 245});
}

void drawBadge(Bitmap& bm, const std::string& label) {
    if (label.empty()) return;
    const float px = 14.0f;
    float w = measureText(label, px, FontFace::UIBold) + 20.0f;
    fillRoundRect(bm, 12, 12, w, 26, 13, Color8{8, 10, 16, 170});
    drawText(bm, 22.0f, 12 + 13 + px * 0.36f, label, px, FontFace::UIBold, Color8{235, 240, 250, 255});
}

void drawPlayGlyph(Bitmap& bm) {
    float cx = bm.w * 0.5f, cy = bm.h * 0.5f - 8.0f, r = 34.0f;
    fillRoundRect(bm, cx - r, cy - r, r * 2, r * 2, r, Color8{8, 10, 16, 150});
    fillTriangle(bm, {cx - 10, cy - 15, cx - 10, cy + 15, cx + 17, cy}, Color8{255, 255, 255, 240});
}

Bitmap composeCard(const Bitmap* src, FileKind kind, const std::string& path, const std::string& name,
                   const std::vector<std::string>* textLines) {
    Bitmap bm;
    bm.w = kThumbW;
    bm.h = kThumbH;
    bm.rgba.assign(static_cast<size_t>(bm.w) * bm.h * 4, 255);
    Color8 top{26, 32, 46, 255}, bottom{11, 14, 22, 255};
    if (kind == FileKind::Audio) { top = {40, 30, 66, 255}; bottom = {14, 12, 30, 255}; }
    if (kind == FileKind::Video) { top = {20, 36, 58, 255}; bottom = {9, 14, 26, 255}; }
    fillBackground(bm, top, bottom);

    if (src && src->w > 0 && src->h > 0) blitCover(bm, src->rgba.data(), src->w, src->h, bottom);

    if (textLines) {
        const float px = 15.0f;
        float y = 62.0f;
        for (const std::string& line : *textLines) {
            if (y > bm.h - 70) break;
            drawText(bm, 16.0f, y, ellipsize(line, px, FontFace::Mono, bm.w - 32.0f), px, FontFace::Mono,
                     Color8{190, 204, 228, 235});
            y += 19.0f;
        }
    }
    if (kind == FileKind::Video || kind == FileKind::Audio) drawPlayGlyph(bm);
    drawScrimAndName(bm, name);
    drawBadge(bm, fileExtensionUpper(path));
    return bm;
}

bool loadImageFile(const std::string& path, Bitmap& out) {
    std::vector<unsigned char> data;
    uintmax_t size = 0;
    if (!readPrefix(path, 96u * 1024 * 1024, data, &size) || size > 96u * 1024 * 1024) return false;
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &comp)) return false;
    if (w <= 0 || h <= 0 || static_cast<int64_t>(w) * h > 120000000) return false;
    unsigned char* px = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &comp, 4);
    if (!px) return false;
    out.w = w;
    out.h = h;
    out.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    return true;
}

bool loadTextPreview(const std::string& path, std::vector<std::string>& lines) {
    std::vector<unsigned char> data;
    if (!readPrefix(path, 4096, data) || data.empty()) return false;
    for (unsigned char c : data)
        if (c == 0) return false;
    std::string cur;
    for (unsigned char c : data) {
        if (c == '\r') continue;
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else if (c == '\t') cur += "    ";
        else cur.push_back(static_cast<char>(c));
        if (lines.size() >= 9) break;
    }
    if (!cur.empty() && lines.size() < 9) lines.push_back(cur);
    return !lines.empty();
}

// ---- On-disk thumbnail cache: finished cards stored as PNG, keyed by path + size + mtime. ----
constexpr const char* kCacheVersion = "v1";

std::filesystem::path cacheDir() {
    namespace fs = std::filesystem;
    fs::path base;
#ifdef _WIN32
    if (const char* p = std::getenv("LOCALAPPDATA")) base = fs::u8path(p) / "SciFiShell";
#else
    if (const char* x = std::getenv("XDG_CACHE_HOME")) base = fs::u8path(x) / "scifishell";
    else if (const char* h = std::getenv("HOME")) base = fs::u8path(h) / ".cache" / "scifishell";
#endif
    if (base.empty()) return {};
    base /= "thumbs";
    std::error_code ec;
    fs::create_directories(base, ec);
    return ec ? fs::path() : base;
}

std::filesystem::path cacheFileFor(const std::filesystem::path& dir, const std::string& path, FileKind kind) {
    namespace fs = std::filesystem;
    if (dir.empty()) return {};
    std::error_code ec;
    fs::path p = fs::u8path(path);
    auto size = fs::file_size(p, ec);
    if (ec) return {};
    auto mtime = fs::last_write_time(p, ec).time_since_epoch().count();
    if (ec) return {};
    std::string key = std::string(kCacheVersion) + '|' + path + '|' + std::to_string(size) + '|' +
                      std::to_string(mtime) + '|' + std::to_string(static_cast<int>(kind)) + '|' +
                      std::to_string(kThumbW) + 'x' + std::to_string(kThumbH);
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.png", static_cast<unsigned long long>(h));
    return dir / name;
}

bool readCache(const std::filesystem::path& file, Bitmap& out) {
    if (file.empty()) return false;
    std::vector<unsigned char> data;
    {
        std::ifstream f(file, std::ios::binary);
        if (!f) return false;
        data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &comp, 4);
    if (!px) return false;
    out.w = w;
    out.h = h;
    out.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    return true;
}

void writeCache(const std::filesystem::path& file, const Bitmap& bm) {
    if (file.empty() || bm.w <= 0 || bm.h <= 0) return;
    std::vector<unsigned char> png;
    auto sink = [](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<unsigned char>*>(ctx);
        v->insert(v->end(), static_cast<unsigned char*>(data), static_cast<unsigned char*>(data) + size);
    };
    if (!stbi_write_png_to_func(sink, &png, bm.w, bm.h, 4, bm.rgba.data(), bm.w * 4)) return;
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return;
        f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);  // atomic publish; a half-written file is never read
    if (ec) std::filesystem::remove(tmp, ec);
}

}  // namespace

FileKind classifyFile(const std::string& path, bool isDir) {
    if (isDir) return FileKind::Folder;
    std::string e = lowerExt(path);
    if (inList(e, {"jpg", "jpeg", "png", "bmp", "gif", "tga"})) return FileKind::Image;
    if (inList(e, {"mp4", "m4v", "mov", "wmv", "avi", "mkv", "webm", "mpg", "mpeg"})) return FileKind::Video;
    if (inList(e, {"mp3", "wav", "flac", "ogg", "m4a", "aac", "wma", "opus"})) return FileKind::Audio;
    if (inList(e, {"txt", "md", "json", "xml", "html", "htm", "css", "js", "ts", "tsx", "jsx", "cpp", "c", "h", "hpp",
                   "py", "rs", "go", "java", "log", "ini", "cfg", "yml", "yaml", "toml", "csv", "cs", "php", "sh",
                   "bat", "ps1", "cmake", "hlsl", "glsl", "sql", "vue", "svg"}))
        return FileKind::Text;
    return FileKind::Other;
}

std::string fileExtensionUpper(const std::string& path) {
    std::string e = lowerExt(path);
    for (char& c : e) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return e.size() > 5 ? e.substr(0, 5) : e;
}

ThumbnailLoader::ThumbnailLoader() : thread_([this] { run(); }) {}

ThumbnailLoader::~ThumbnailLoader() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

int ThumbnailLoader::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.clear();
    results_.clear();
    return ++generation_;
}

void ThumbnailLoader::request(int generation, int index, const std::string& path, const std::string& name, FileKind kind) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back({generation, index, path, name, kind});
    }
    cv_.notify_one();
}

bool ThumbnailLoader::poll(ThumbResult& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!results_.empty()) {
        ThumbResult r = std::move(results_.front());
        results_.pop_front();
        if (r.generation != generation_.load()) continue;
        out = std::move(r);
        return true;
    }
    return false;
}

void ThumbnailLoader::run() {
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
    const std::filesystem::path cacheDirectory = cacheDir();
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
            if (quit_) break;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        if (job.generation != generation_.load()) continue;

        const std::filesystem::path cacheFile = cacheFileFor(cacheDirectory, job.path, job.kind);
        {
            Bitmap cached;
            if (readCache(cacheFile, cached)) {
                ThumbResult res;
                res.generation = job.generation;
                res.index = job.index;
                res.bitmap = std::move(cached);
                std::lock_guard<std::mutex> lock(mutex_);
                results_.push_back(std::move(res));
                continue;
            }
        }

        Bitmap frame;
        std::vector<std::string> lines;
        bool haveFrame = false, haveLines = false;
        try {
            if (job.kind == FileKind::Image) haveFrame = loadImageFile(job.path, frame);
            else if (job.kind == FileKind::Video) haveFrame = extractVideoFrame(job.path, frame);
            else if (job.kind == FileKind::Text) haveLines = loadTextPreview(job.path, lines);
        } catch (...) {
        }
        // Audio, undecodable media and non-text "text" files still get a typed card.
        if (job.kind == FileKind::Text && !haveLines) continue;
        if (job.kind == FileKind::Image && !haveFrame) continue;

        ThumbResult res;
        res.generation = job.generation;
        res.index = job.index;
        res.bitmap = composeCard(haveFrame ? &frame : nullptr, job.kind, job.path, job.name, haveLines ? &lines : nullptr);
        writeCache(cacheFile, res.bitmap);
        std::lock_guard<std::mutex> lock(mutex_);
        results_.push_back(std::move(res));
    }
#ifdef _WIN32
    CoUninitialize();
#endif
}

}  // namespace sf
