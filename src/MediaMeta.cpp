#include "MediaMeta.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "stb_image.h"
#include "Thumbnails.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shobjidl.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

namespace sf {

namespace fs = std::filesystem;

Bitmap scaleCover(const Bitmap& s, int W, int H) {
    Bitmap o;
    o.w = W;
    o.h = H;
    o.rgba.assign(static_cast<size_t>(W) * H * 4, 255);
    if (s.w <= 0 || s.h <= 0 || s.rgba.size() < static_cast<size_t>(s.w) * s.h * 4) return o;
    const float sc = std::max(static_cast<float>(W) / s.w, static_cast<float>(H) / s.h);
    const float ox = (s.w - W / sc) * 0.5f, oy = (s.h - H / sc) * 0.5f;
    const int taps = std::clamp(static_cast<int>(std::ceil(1.0f / sc)), 1, 6);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float acc[3] = {0, 0, 0};
            for (int j = 0; j < taps; ++j)
                for (int i = 0; i < taps; ++i) {
                    const float fx = ox + (x + (i + 0.5f) / taps) / sc, fy = oy + (y + (j + 0.5f) / taps) / sc;
                    const int sx = std::clamp(static_cast<int>(fx), 0, s.w - 1), sy = std::clamp(static_cast<int>(fy), 0, s.h - 1);
                    const uint8_t* p = &s.rgba[(static_cast<size_t>(sy) * s.w + sx) * 4];
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                }
            uint8_t* d = &o.rgba[(static_cast<size_t>(y) * W + x) * 4];
            const float n = static_cast<float>(taps * taps);
            d[0] = static_cast<uint8_t>(acc[0] / n);
            d[1] = static_cast<uint8_t>(acc[1] / n);
            d[2] = static_cast<uint8_t>(acc[2] / n);
            d[3] = 255;
        }
    return o;
}

namespace {

bool decodeImageFile(const fs::path& p, Bitmap& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.empty() || data.size() > 24u * 1024 * 1024) return false;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &n, 4);
    if (!px) return false;
    out.w = w;
    out.h = h;
    out.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    return true;
}

bool folderArt(const std::string& trackPath, Bitmap& out) {
    std::error_code ec;
    const fs::path dir = fs::u8path(trackPath).parent_path();
    for (const char* base : {"folder", "cover", "front", "album", "albumart", "albumartsmall", "artwork"})
        for (const char* ext : {".jpg", ".jpeg", ".png"}) {
            const fs::path p = dir / (std::string(base) + ext);
            if (fs::is_regular_file(p, ec) && decodeImageFile(p, out)) return true;
        }
    return false;
}

#ifdef _WIN32
std::wstring widen(const std::string& u8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 1)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, w.data(), n);
    w.resize(n > 0 ? static_cast<size_t>(n - 1) : 0);
    return w;
}

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string propString(IPropertyStore* ps, const PROPERTYKEY& key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string out;
    if (SUCCEEDED(ps->GetValue(key, &v))) {
        PWSTR s = nullptr;
        if ((v.vt & VT_VECTOR) ? SUCCEEDED(PropVariantGetStringElem(v, 0, &s)) : SUCCEEDED(PropVariantToStringAlloc(v, &s))) {
            if (s) out = narrow(s);
            CoTaskMemFree(s);
        }
    }
    PropVariantClear(&v);
    return out;
}

unsigned long long propNumber(IPropertyStore* ps, const PROPERTYKEY& key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    unsigned long long out = 0;
    if (SUCCEEDED(ps->GetValue(key, &v))) PropVariantToUInt64(v, &out);
    PropVariantClear(&v);
    return out;
}

bool readTags(const std::string& path, TrackMeta& m) {
    ComPtr<IPropertyStore> ps;
    if (FAILED(SHGetPropertyStoreFromParsingName(widen(path).c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&ps)))) return false;
    m.title = propString(ps.Get(), PKEY_Title);
    m.artist = propString(ps.Get(), PKEY_Music_Artist);
    if (m.artist.empty()) m.artist = propString(ps.Get(), PKEY_Author);
    m.albumArtist = propString(ps.Get(), PKEY_Music_AlbumArtist);
    m.album = propString(ps.Get(), PKEY_Music_AlbumTitle);
    m.track = static_cast<int>(propNumber(ps.Get(), PKEY_Music_TrackNumber));
    m.year = static_cast<int>(propNumber(ps.Get(), PKEY_Media_Year));
    m.seconds = static_cast<double>(propNumber(ps.Get(), PKEY_Media_Duration)) / 1e7;
    return true;
}

bool shellThumb(const std::string& path, int size, Bitmap& out) {
    ComPtr<IShellItemImageFactory> f;
    if (FAILED(SHCreateItemFromParsingName(widen(path).c_str(), nullptr, IID_PPV_ARGS(&f)))) return false;
    HBITMAP hb = nullptr;
    const SIZE sz{size, size};
    if (FAILED(f->GetImage(sz, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &hb)) || !hb) return false;
    BITMAP bmp{};
    bool ok = false;
    if (GetObject(hb, sizeof bmp, &bmp) && bmp.bmWidth > 0 && bmp.bmHeight > 0) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = bmp.bmWidth;
        bi.bmiHeader.biHeight = -bmp.bmHeight;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        out.w = bmp.bmWidth;
        out.h = bmp.bmHeight;
        out.rgba.assign(static_cast<size_t>(out.w) * out.h * 4, 0);
        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, hb, 0, static_cast<UINT>(out.h), out.rgba.data(), &bi, DIB_RGB_COLORS) > 0;
        ReleaseDC(nullptr, dc);
        if (ok)
            for (size_t i = 0; i < out.rgba.size(); i += 4) {
                std::swap(out.rgba[i], out.rgba[i + 2]);
                out.rgba[i + 3] = 255;
            }
    }
    DeleteObject(hb);
    return ok;
}
#endif

}  // namespace

MediaMeta::MediaMeta() { thread_ = std::thread([this] { run(); }); }

MediaMeta::~MediaMeta() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void MediaMeta::requestTags(const std::string& path, bool urgent) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        (urgent ? urgentJobs_ : tagJobs_).push_back({0, path, path});
    }
    cv_.notify_one();
}

void MediaMeta::requestArt(const std::string& key, const std::string& path) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        picJobs_.push_back({1, key, path});
    }
    cv_.notify_one();
}

void MediaMeta::requestThumb(const std::string& key, const std::string& path) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        picJobs_.push_back({2, key, path});
    }
    cv_.notify_one();
}

bool MediaMeta::poll(MetaResult& out) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (results_.empty()) return false;
    out = std::move(results_.front());
    results_.pop_front();
    return true;
}

size_t MediaMeta::pending() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return tagJobs_.size() + urgentJobs_.size() + picJobs_.size() + results_.size() + (busy_ ? 1 : 0);
}

void MediaMeta::run() {
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [&] { return quit_ || !tagJobs_.empty() || !urgentJobs_.empty() || !picJobs_.empty(); });
            if (quit_) break;
            if (!urgentJobs_.empty()) {
                job = std::move(urgentJobs_.back());  // newest first: what is on screen now
                urgentJobs_.pop_back();
            } else if (!picJobs_.empty()) {
                job = std::move(picJobs_.back());
                picJobs_.pop_back();
            } else {
                job = std::move(tagJobs_.front());
                tagJobs_.pop_front();
            }
            busy_ = true;
        }
        MetaResult r;
        r.key = job.key;
        try {
            if (job.kind == 0) {
                r.kind = MetaResult::Tags;
#ifdef _WIN32
                r.ok = readTags(job.path, r.meta);
#endif
            } else if (job.kind == 1) {
                r.kind = MetaResult::Art;
                Bitmap raw;
                bool have = folderArt(job.path, raw);
#ifdef _WIN32
                if (!have) have = shellThumb(job.path, 320, raw);
#endif
                if (have) {
                    r.bmp = scaleCover(raw, 320, 320);
                    r.ok = true;
                }
            } else {
                r.kind = MetaResult::Thumb;
                Bitmap raw;
                if (extractVideoFrame(job.path, raw)) {
                    r.bmp = scaleCover(raw, 384, 216);
                    r.ok = true;
                }
            }
        } catch (...) {
        }
        std::lock_guard<std::mutex> lk(mutex_);
        results_.push_back(std::move(r));
        busy_ = false;
    }
#ifdef _WIN32
    CoUninitialize();
#endif
}

}  // namespace sf
