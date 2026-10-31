#include "MediaPlayer.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <SDL.h>
#include <windows.h>
#include <d3d11_4.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace sf {

namespace {
constexpr double kBytesPerSec = 48000.0 * 2 * 2;  // 48 kHz, stereo, 16-bit
constexpr size_t kVideoQueue = 6;                 // decoded frames kept ahead of the clock
constexpr double kAudioAhead = 0.3;               // seconds of audio kept queued
using Clock = std::chrono::steady_clock;

// In-place radix-2 FFT for the fixed spectrum size, with twiddle factors, bit-reversal and window computed once.
constexpr int kFftN = 1024;
struct FftTables {
    float cosT[kFftN / 2], sinT[kFftN / 2];
    uint16_t rev[kFftN];
    float window[kFftN];
    int k0[64], k1[64];
    int bands = 0;
    FftTables() {
        for (int i = 0; i < kFftN / 2; ++i) {
            cosT[i] = std::cos(-6.2831853f * i / kFftN);
            sinT[i] = std::sin(-6.2831853f * i / kFftN);
        }
        for (int i = 0, j = 0; i < kFftN; ++i) {
            rev[i] = static_cast<uint16_t>(j);
            int bit = kFftN >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
        }
        for (int i = 0; i < kFftN; ++i) window[i] = 0.5f - 0.5f * std::cos(6.2831853f * i / (kFftN - 1));
    }
    void setBands(int nb) {
        if (nb == bands || nb > 64) return;
        bands = nb;
        const float lo = 50.0f, hi = 16000.0f, binHz = 48000.0f / kFftN;
        for (int b = 0; b < nb; ++b) {
            const float f0 = lo * std::pow(hi / lo, static_cast<float>(b) / nb), f1 = lo * std::pow(hi / lo, static_cast<float>(b + 1) / nb);
            k0[b] = std::clamp(static_cast<int>(f0 / binHz), 1, kFftN / 2 - 1);
            k1[b] = std::clamp(static_cast<int>(f1 / binHz), k0[b], kFftN / 2 - 1);
        }
    }
};

void fft(float* re, float* im, const FftTables& T) {
    for (int i = 0; i < kFftN; ++i) {
        const int j = T.rev[i];
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (int len = 2; len <= kFftN; len <<= 1) {
        const int half = len / 2, step = kFftN / len;
        for (int i = 0; i < kFftN; i += len) {
            for (int k = 0; k < half; ++k) {
                const float cr = T.cosT[k * step], ci = T.sinT[k * step];
                const int a = i + k, b = a + half;
                const float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}}  // namespace

struct MediaPlayer::Impl {
    struct Frame {
        double pts = 0;
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };

    mutable std::mutex m;
    std::condition_variable cv;
    std::thread thread;
    bool quit = false, openDone = false, openOk = false, go = false;
    std::string path;

    // Stream info (written by the worker before openDone; the video size can change mid-stream).
    bool hasV = false, hasA = false;
    int vw = 0, vh = 0;
    int cw = 0, ch = 0;
    double dur = 0;

    // Playback state, guarded by m.
    bool playing = false, seekPending = false, wantFrame = false, eos = false;
    double seekTarget = 0, seekBase = 0, dropBefore = 0;
    uint64_t epoch = 0;
    float volume = 1.0f;
    std::deque<Frame> frames;

    // Audio: the audio device queue is the master clock.
    SDL_AudioDeviceID dev = 0;
    uint64_t audioQueued = 0;
    double audioBase = 0;
    bool audioBaseSet = false;
    std::deque<std::pair<uint64_t, std::vector<int16_t>>> hist;  // recent unscaled PCM and its byte offset, for the spectrum
    size_t histBytes = 0;

    // Clock for files without audio.
    double clockBase = 0;
    Clock::time_point wallStart;

    // Worker-only.
    ComPtr<IMFSourceReader> reader;
    int vIdx = -1, aIdx = -1;
    LONG vStride = 0;
    bool vEos = false, aEos = false;
    std::atomic<int> maxW{0}, maxH{0};

    // Hardware decode: the decoder (DXVA) writes NV12 surfaces on its own D3D11 device; only the picture at its displayed
    // size is read back and colour-converted on the CPU.
    bool hw = false;
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> d3dCtx;
    ComPtr<IMFDXGIDeviceManager> dxgiMgr;
    ComPtr<ID3D11Texture2D> staging;

    bool createDevice() {
        if (d3d) return true;
        const D3D_FEATURE_LEVEL lv[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     lv, 2, D3D11_SDK_VERSION, &d3d, nullptr, &d3dCtx)))
            return false;
        ComPtr<ID3D11Multithread> mt;
        if (SUCCEEDED(d3d.As(&mt))) mt->SetMultithreadProtected(TRUE);
        UINT token = 0;
        if (FAILED(MFCreateDXGIDeviceManager(&token, &dxgiMgr)) || FAILED(dxgiMgr->ResetDevice(d3d.Get(), token))) return false;
        return true;
    }

    void resetStream() {
        reader.Reset();
        staging.Reset();
        vIdx = aIdx = -1;
        hasV = hasA = false;
        vw = vh = cw = ch = 0;
        vStride = 0;
        dur = 0;
    }

    // Reads the NV12 surface behind a hardware-decoded sample into a (possibly downscaled) RGBA frame.
    bool convertHw(IMFSample* sample, Frame& f) {
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->GetBufferByIndex(0, &buf))) return false;
        ComPtr<IMFDXGIBuffer> dx;
        if (FAILED(buf.As(&dx))) return false;
        ComPtr<ID3D11Texture2D> tex;
        UINT sub = 0;
        if (FAILED(dx->GetResource(IID_PPV_ARGS(&tex))) || FAILED(dx->GetSubresourceIndex(&sub))) return false;
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        if (d.Format != DXGI_FORMAT_NV12) return false;
        bool fresh = !staging;
        if (staging) {
            D3D11_TEXTURE2D_DESC sd{};
            staging->GetDesc(&sd);
            fresh = sd.Width != d.Width || sd.Height != d.Height;
        }
        if (fresh) {
            staging.Reset();
            D3D11_TEXTURE2D_DESC sd = d;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            sd.ArraySize = 1;
            sd.MipLevels = 1;
            if (FAILED(d3d->CreateTexture2D(&sd, nullptr, &staging))) return false;
        }
        int w, h;
        {
            std::lock_guard<std::mutex> lk(m);
            w = vw;
            h = vh;
        }
        if (w <= 0 || h <= 0 || static_cast<UINT>(w) > d.Width || static_cast<UINT>(h) > d.Height) return false;
        d3dCtx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex.Get(), sub, nullptr);
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (FAILED(d3dCtx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &ms))) return false;
        const uint8_t* yp = static_cast<const uint8_t*>(ms.pData);
        const uint8_t* uvp = yp + static_cast<size_t>(ms.RowPitch) * d.Height;

        int ow = w, oh = h;
        const int mw = maxW.load(), mh = maxH.load();
        if (mw > 0 && mh > 0) {
            const double s = std::min(1.0, std::min(static_cast<double>(mw) / w, static_cast<double>(mh) / h));
            ow = std::max(2, static_cast<int>(w * s) & ~1);
            oh = std::max(2, static_cast<int>(h * s) & ~1);
        }
        f.w = ow;
        f.h = oh;
        f.rgba.resize(static_cast<size_t>(ow) * oh * 4);
        const bool hd = h >= 720 || w >= 1280;
        const int crv = hd ? 459 : 409, cgu = hd ? 55 : 100, cgv = hd ? 137 : 208, cbu = hd ? 541 : 516;
        const double fx = static_cast<double>(w) / ow, fy = static_cast<double>(h) / oh;
        std::vector<int> xa(static_cast<size_t>(ow)), xb(static_cast<size_t>(ow));
        for (int x = 0; x < ow; ++x) {
            xa[static_cast<size_t>(x)] = std::min(w - 1, static_cast<int>((x + 0.25) * fx));
            xb[static_cast<size_t>(x)] = std::min(w - 1, static_cast<int>((x + 0.75) * fx));
        }
        for (int y = 0; y < oh; ++y) {
            const int ya = std::min(h - 1, static_cast<int>((y + 0.25) * fy)), yb = std::min(h - 1, static_cast<int>((y + 0.75) * fy));
            const uint8_t* r0 = yp + static_cast<size_t>(ya) * ms.RowPitch;
            const uint8_t* r1 = yp + static_cast<size_t>(yb) * ms.RowPitch;
            const uint8_t* cr = uvp + static_cast<size_t>((ya + yb) / 4) * ms.RowPitch;
            uint8_t* o = &f.rgba[static_cast<size_t>(y) * ow * 4];
            for (int x = 0; x < ow; ++x, o += 4) {
                const int a = xa[static_cast<size_t>(x)], b = xb[static_cast<size_t>(x)];
                const int c = ((r0[a] + r0[b] + r1[a] + r1[b]) >> 2) - 16;
                const int ci = ((a + b) >> 2) * 2;
                const int u = cr[ci] - 128, v = cr[ci + 1] - 128;
                const int yy = 298 * c + 128;
                o[0] = static_cast<uint8_t>(std::clamp((yy + crv * v) >> 8, 0, 255));
                o[1] = static_cast<uint8_t>(std::clamp((yy - cgu * u - cgv * v) >> 8, 0, 255));
                o[2] = static_cast<uint8_t>(std::clamp((yy + cbu * u) >> 8, 0, 255));
                o[3] = 255;
            }
        }
        d3dCtx->Unmap(staging.Get(), 0);
        return true;
    }

    // Decodes the first frame through the hardware path, to be sure it works before committing to it.
    bool probeHw() {
        for (int tries = 0; tries < 40; ++tries) {
            DWORD si = 0, fl = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> s;
            if (FAILED(reader->ReadSample(static_cast<DWORD>(vIdx), 0, &si, &fl, &ts, &s)) || (fl & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)))
                return false;
            if (fl & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) refreshVideoType();
            if (!s) continue;
            Frame f;
            if (!convertHw(s.Get(), f)) return false;
            PROPVARIANT pos;
            PropVariantInit(&pos);
            pos.vt = VT_I8;
            pos.hVal.QuadPart = 0;
            reader->SetCurrentPosition(GUID_NULL, pos);
            return true;
        }
        return false;
    }
    ~Impl() { stop(); }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(m);
            quit = true;
        }
        cv.notify_all();
        if (thread.joinable()) thread.join();
        if (dev) {
            SDL_CloseAudioDevice(dev);
            dev = 0;
        }
    }

    double clockLocked() const {
        if (hasA && dev) {
            if (!audioBaseSet) return seekBase;
            Uint32 q = SDL_GetQueuedAudioSize(dev);
            double played = audioQueued > q ? static_cast<double>(audioQueued - q) / kBytesPerSec : 0.0;
            return audioBase + played;
        }
        if (!playing) return clockBase;
        return clockBase + std::chrono::duration<double>(Clock::now() - wallStart).count();
    }

    bool refreshVideoType() {
        ComPtr<IMFMediaType> type;
        if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(vIdx), &type))) return false;
        UINT32 w = 0, h = 0;
        if (FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h)) || !w || !h) return false;
        UINT32 raw = 0;
        LONG stride;
        if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw))) stride = static_cast<LONG>(static_cast<INT32>(raw));
        else if (FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, w, &stride))) stride = static_cast<LONG>(w * 4);
        std::lock_guard<std::mutex> lk(m);
        // The public size is fixed by the first (display) size; later types may be padded to 16-row multiples.
        if (!vw || !vh) {
            vw = static_cast<int>(w);
            vh = static_cast<int>(h);
        }
        cw = static_cast<int>(w);
        ch = static_cast<int>(h);
        vStride = stride;
        return true;
    }

    bool openReader(bool tryHw) {
        hw = false;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (wlen <= 0) return false;
        std::wstring wpath(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);

        ComPtr<IMFAttributes> attrs;
        if (FAILED(MFCreateAttributes(&attrs, 4))) return false;
        if (tryHw && createDevice()) {
            attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dxgiMgr.Get());
            attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            hw = true;
        } else {
            attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        }
        if (FAILED(MFCreateSourceReaderFromURL(wpath.c_str(), attrs.Get(), &reader))) return false;

        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> t;
            if (FAILED(reader->GetNativeMediaType(i, 0, &t))) break;
            GUID major{};
            t->GetGUID(MF_MT_MAJOR_TYPE, &major);
            if (major == MFMediaType_Video && vIdx < 0) vIdx = static_cast<int>(i);
            if (major == MFMediaType_Audio && aIdx < 0) aIdx = static_cast<int>(i);
        }
        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);

        if (vIdx >= 0) {
            ComPtr<IMFMediaType> want;
            MFCreateMediaType(&want);
            want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            want->SetGUID(MF_MT_SUBTYPE, hw ? MFVideoFormat_NV12 : MFVideoFormat_RGB32);
            if (SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(vIdx), nullptr, want.Get())) && refreshVideoType()) {
                reader->SetStreamSelection(static_cast<DWORD>(vIdx), TRUE);
                hasV = true;
            }
        }
        if (aIdx >= 0) {
            ComPtr<IMFMediaType> want;
            MFCreateMediaType(&want);
            want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            want->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            want->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            want->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
            want->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);
            want->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
            if (SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(aIdx), nullptr, want.Get()))) {
                reader->SetStreamSelection(static_cast<DWORD>(aIdx), TRUE);
                hasA = true;
            }
        }
        if (!hasV && !hasA) return false;

        PROPVARIANT d;
        PropVariantInit(&d);
        if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &d)) &&
            d.vt == VT_UI8)
            dur = static_cast<double>(d.uhVal.QuadPart) / 1e7;
        PropVariantClear(&d);
        return true;
    }

    void queueVideo(IMFSample* sample, double pts) {
        if (hw) {
            Frame f;
            f.pts = pts;
            if (!convertHw(sample, f)) return;
            std::lock_guard<std::mutex> lk(m);
            frames.push_back(std::move(f));
            if (frames.size() > kVideoQueue * 4) frames.pop_front();
            wantFrame = false;
            return;
        }        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return;
        int w, h, cwid, chgt;
        LONG stride;
        {
            std::lock_guard<std::mutex> lk(m);
            w = vw;
            h = vh;
            cwid = cw;
            chgt = ch;
            stride = vStride;
        }
        BYTE* data = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(buffer->Lock(&data, &maxLen, &curLen))) return;
        size_t absStride = static_cast<size_t>(std::labs(stride));
        if (cwid >= w && chgt >= h && absStride >= static_cast<size_t>(w) * 4 && curLen >= absStride * chgt) {
            Frame f;
            f.pts = pts;
            int ow = w, oh = h;
            const int mw = maxW.load(), mh = maxH.load();
            if (mw > 0 && mh > 0) {
                const double s = std::min(1.0, std::min(static_cast<double>(mw) / w, static_cast<double>(mh) / h));
                ow = std::max(2, static_cast<int>(w * s) & ~1);
                oh = std::max(2, static_cast<int>(h * s) & ~1);
            }
            f.w = ow;
            f.h = oh;
            f.rgba.resize(static_cast<size_t>(ow) * oh * 4);
            auto rowPtr = [&](int y) { return data + static_cast<size_t>(stride < 0 ? (chgt - 1 - y) : y) * absStride; };
            if (ow == w && oh == h) {
                for (int y = 0; y < h; ++y) {
                    const BYTE* srow = rowPtr(y);
                    uint8_t* drow = &f.rgba[static_cast<size_t>(y) * w * 4];
                    for (int x = 0; x < w; ++x) {
                        drow[x * 4 + 0] = srow[x * 4 + 2];
                        drow[x * 4 + 1] = srow[x * 4 + 1];
                        drow[x * 4 + 2] = srow[x * 4 + 0];
                        drow[x * 4 + 3] = 255;
                    }
                }
            } else {
                // Four taps per output pixel, spread over its footprint: cheap and clean enough for moving pictures.
                const double fx = static_cast<double>(w) / ow, fy = static_cast<double>(h) / oh;
                std::vector<int> xa(static_cast<size_t>(ow)), xb(static_cast<size_t>(ow));
                for (int x = 0; x < ow; ++x) {
                    xa[static_cast<size_t>(x)] = std::min(w - 1, static_cast<int>((x + 0.25) * fx)) * 4;
                    xb[static_cast<size_t>(x)] = std::min(w - 1, static_cast<int>((x + 0.75) * fx)) * 4;
                }
                for (int y = 0; y < oh; ++y) {
                    const BYTE* r0 = rowPtr(std::min(h - 1, static_cast<int>((y + 0.25) * fy)));
                    const BYTE* r1 = rowPtr(std::min(h - 1, static_cast<int>((y + 0.75) * fy)));
                    uint8_t* d = &f.rgba[static_cast<size_t>(y) * ow * 4];
                    for (int x = 0; x < ow; ++x, d += 4) {
                        const int a = xa[static_cast<size_t>(x)], b = xb[static_cast<size_t>(x)];
                        d[0] = static_cast<uint8_t>((r0[a + 2] + r0[b + 2] + r1[a + 2] + r1[b + 2]) >> 2);
                        d[1] = static_cast<uint8_t>((r0[a + 1] + r0[b + 1] + r1[a + 1] + r1[b + 1]) >> 2);
                        d[2] = static_cast<uint8_t>((r0[a] + r0[b] + r1[a] + r1[b]) >> 2);
                        d[3] = 255;
                    }
                }
            }
            std::lock_guard<std::mutex> lk(m);
            frames.push_back(std::move(f));
            if (frames.size() > kVideoQueue * 4) frames.pop_front();
            wantFrame = false;
        }
        buffer->Unlock();
    }

    void queueAudio(IMFSample* sample, double pts) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return;
        BYTE* data = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(buffer->Lock(&data, &maxLen, &curLen))) return;
        std::vector<int16_t> pcm(curLen / 2);
        std::memcpy(pcm.data(), data, pcm.size() * 2);
        buffer->Unlock();
        std::lock_guard<std::mutex> lk(m);
        hist.emplace_back(audioQueued, pcm);
        histBytes += pcm.size() * 2;
        while (histBytes > 600000 && hist.size() > 1) {
            histBytes -= hist.front().second.size() * 2;
            hist.pop_front();
        }
        if (volume < 0.999f)
            for (auto& s : pcm) s = static_cast<int16_t>(s * volume);
        if (!audioBaseSet) {
            audioBase = pts;
            audioBaseSet = true;
        }
        SDL_QueueAudio(dev, pcm.data(), static_cast<Uint32>(pcm.size() * 2));
        audioQueued += pcm.size() * 2;
    }

    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
        bool ok = false;
        if (mf) {
            const bool allowHw = !std::getenv("SCIFI_SW_VIDEO");
            ok = openReader(allowHw);
            if (ok && hw && hasV && !probeHw()) {
                resetStream();
                ok = openReader(false);
            }
        }        {
            std::lock_guard<std::mutex> lk(m);
            openOk = ok;
            openDone = true;
        }
        cv.notify_all();
        if (ok) {
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return go || quit; });
            }
            loop();
        }
        reader.Reset();
        staging.Reset();
        dxgiMgr.Reset();
        d3dCtx.Reset();
        d3d.Reset();
        if (mf) MFShutdown();
        CoUninitialize();
    }

    void loop() {
        for (;;) {
            bool doSeek = false;
            double target = 0;
            bool active;
            double audioAhead = 99;
            size_t nframes;
            uint64_t myEpoch;
            {
                std::lock_guard<std::mutex> lk(m);
                if (quit) return;
                if (seekPending) {
                    doSeek = true;
                    target = seekTarget;
                    seekPending = false;
                }
                active = (playing || wantFrame) && !eos;
                if (hasA && dev) audioAhead = static_cast<double>(SDL_GetQueuedAudioSize(dev)) / kBytesPerSec;
                nframes = frames.size();
                myEpoch = epoch;
            }
            if (doSeek) {
                PROPVARIANT pos;
                PropVariantInit(&pos);
                pos.vt = VT_I8;
                pos.hVal.QuadPart = static_cast<LONGLONG>(target * 1e7);
                reader->SetCurrentPosition(GUID_NULL, pos);
                vEos = aEos = false;
                continue;
            }
            bool needV = hasV && nframes < kVideoQueue;
            bool needA = hasA && dev && audioAhead < kAudioAhead;
            if (!active || !(needV || needA)) {
                std::unique_lock<std::mutex> lk(m);
                cv.wait_for(lk, std::chrono::milliseconds(5));
                continue;
            }

            DWORD si = 0, fl = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> sample;
            HRESULT hr = reader->ReadSample(MF_SOURCE_READER_ANY_STREAM, 0, &si, &fl, &ts, &sample);
            double minPts;
            {
                std::lock_guard<std::mutex> lk(m);
                if (epoch != myEpoch) continue;  // a seek happened while we were blocked in ReadSample
                if (FAILED(hr) || (fl & MF_SOURCE_READERF_ERROR)) {
                    eos = true;
                    continue;
                }
                if (fl & MF_SOURCE_READERF_ENDOFSTREAM) {
                    if (static_cast<int>(si) == vIdx) vEos = true;
                    else if (static_cast<int>(si) == aIdx) aEos = true;
                    eos = (!hasV || vEos) && (!hasA || aEos);
                }
                minPts = dropBefore;
            }
            if ((fl & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) && static_cast<int>(si) == vIdx) refreshVideoType();
            if (!sample) continue;
            double pts = static_cast<double>(ts) / 1e7;
            if (static_cast<int>(si) == vIdx && hasV) {
                if (pts >= minPts - 0.002) queueVideo(sample.Get(), pts);
            } else if (static_cast<int>(si) == aIdx && hasA && dev) {
                if (pts >= minPts - 0.25) queueAudio(sample.Get(), pts);
            }
        }
    }
};

MediaPlayer::MediaPlayer() = default;
MediaPlayer::~MediaPlayer() = default;
MediaPlayer::MediaPlayer(MediaPlayer&&) noexcept = default;
MediaPlayer& MediaPlayer::operator=(MediaPlayer&&) noexcept = default;

bool MediaPlayer::open(const std::string& path) {
    close();
    impl_ = std::make_unique<Impl>();
    Impl& I = *impl_;
    I.path = path;
    I.thread = std::thread([&I] { I.run(); });
    bool ok;
    {
        std::unique_lock<std::mutex> lk(I.m);
        I.cv.wait(lk, [&] { return I.openDone; });
        ok = I.openOk;
    }
    if (!ok) {
        impl_.reset();
        return false;
    }
    if (I.hasA) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
            SDL_AudioSpec want{}, have{};
            want.freq = 48000;
            want.format = AUDIO_S16SYS;
            want.channels = 2;
            want.samples = 1024;
            I.dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        }
        if (!I.dev) std::fprintf(stderr, "Audio output unavailable: %s\n", SDL_GetError());
    }
    {
        std::lock_guard<std::mutex> lk(I.m);
        I.wantFrame = I.hasV;
        I.go = true;
    }
    I.cv.notify_all();
    return true;
}

void MediaPlayer::close() { impl_.reset(); }

bool MediaPlayer::isOpen() const { return impl_ != nullptr; }
bool MediaPlayer::hasVideo() const { return impl_ && impl_->hasV; }
bool MediaPlayer::hasAudio() const { return impl_ && impl_->hasA; }
int MediaPlayer::videoWidth() const { return impl_ ? impl_->vw : 0; }
int MediaPlayer::videoHeight() const { return impl_ ? impl_->vh : 0; }
double MediaPlayer::duration() const { return impl_ ? impl_->dur : 0.0; }

void MediaPlayer::play() {
    if (!impl_) return;
    Impl& I = *impl_;
    {
        std::lock_guard<std::mutex> lk(I.m);
        if (I.playing) return;
        I.playing = true;
        I.wallStart = Clock::now();
        if (I.dev) SDL_PauseAudioDevice(I.dev, 0);
    }
    I.cv.notify_all();
}

void MediaPlayer::pause() {
    if (!impl_) return;
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.m);
    if (!I.playing) return;
    if (!(I.hasA && I.dev)) I.clockBase = I.clockLocked();
    I.playing = false;
    if (I.dev) SDL_PauseAudioDevice(I.dev, 1);
}

bool MediaPlayer::playing() const {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->playing;
}

void MediaPlayer::seek(double seconds) {
    if (!impl_) return;
    Impl& I = *impl_;
    {
        std::lock_guard<std::mutex> lk(I.m);
        if (I.dur > 0) seconds = std::min(seconds, I.dur);
        seconds = std::max(seconds, 0.0);
        ++I.epoch;
        I.seekPending = true;
        I.seekTarget = seconds;
        I.seekBase = seconds;
        I.dropBefore = seconds;
        I.frames.clear();
        if (I.dev) SDL_ClearQueuedAudio(I.dev);
        I.audioQueued = 0;
        I.hist.clear();
        I.histBytes = 0;
        I.audioBaseSet = false;
        I.clockBase = seconds;
        I.wallStart = Clock::now();
        I.eos = false;
        I.wantFrame = I.hasV;
    }
    I.cv.notify_all();
}

double MediaPlayer::position() const {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lk(impl_->m);
    double t = impl_->clockLocked();
    if (impl_->dur > 0) t = std::min(t, impl_->dur);
    return std::max(t, 0.0);
}

void MediaPlayer::setVolume(float v) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->volume = std::min(1.0f, std::max(0.0f, v));
}

float MediaPlayer::volume() const {
    if (!impl_) return 1.0f;
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->volume;
}

bool MediaPlayer::ended() const {
    if (!impl_) return false;
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.m);
    if (!I.eos || I.seekPending || !I.frames.empty()) return false;
    if (I.hasA && I.dev && SDL_GetQueuedAudioSize(I.dev) > 0) return false;
    return true;
}

bool MediaPlayer::spectrum(float* bands, int nb) {
    if (!impl_ || nb <= 0) return false;
    Impl& I = *impl_;
    constexpr int N = kFftN;
    static FftTables T;
    T.setBands(nb);
    float re[N] = {}, im[N] = {};
    {
        std::lock_guard<std::mutex> lk(I.m);
        if (!I.hasA || !I.dev || !I.playing) return false;
        const Uint32 q = SDL_GetQueuedAudioSize(I.dev);
        const uint64_t played = I.audioQueued > q ? I.audioQueued - q : 0;
        const uint64_t start = played >= N * 4 ? played - N * 4 : 0;
        for (const auto& c : I.hist) {
            const uint64_t s = c.first, e = s + c.second.size() * 2;
            if (e <= start || s >= start + N * 4) continue;
            for (uint64_t b = std::max(s, start); b + 3 < std::min(e, start + N * 4); b += 4) {
                const size_t si = static_cast<size_t>((b - s) / 2);
                const size_t di = static_cast<size_t>((b - start) / 4);
                if (di < N && si + 1 < c.second.size()) re[di] = (c.second[si] + c.second[si + 1]) * (0.5f / 32768.0f);
            }
        }
        while (I.hist.size() > 2 && I.hist.front().first + I.hist.front().second.size() * 2 < start) {
            I.histBytes -= I.hist.front().second.size() * 2;
            I.hist.pop_front();
        }
    }
    for (int i = 0; i < N; ++i) re[i] *= T.window[i];
    fft(re, im, T);
    for (int b = 0; b < nb; ++b) {
        float m2 = 0;
        for (int k = T.k0[b]; k <= T.k1[b]; ++k) m2 = std::max(m2, re[k] * re[k] + im[k] * im[k]);
        const float mag = std::sqrt(m2);
        const float db = 20.0f * std::log10(mag / (N * 0.25f) + 1e-6f) + static_cast<float>(b) * 0.45f;
        bands[b] = std::clamp((db + 62.0f) / 52.0f, 0.0f, 1.0f);
    }
    return true;
}

void MediaPlayer::setMaxVideoSize(int w, int h) {
    if (!impl_) return;
    impl_->maxW = w;
    impl_->maxH = h;
}

bool MediaPlayer::takeFrame(std::vector<uint8_t>& rgba) {
    int w, h;
    return takeFrame(rgba, w, h);
}

bool MediaPlayer::takeFrame(std::vector<uint8_t>& rgba, int& fw, int& fh) {
    if (!impl_) return false;
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.m);
    bool got = false;
    double clk = I.clockLocked();
    while (!I.frames.empty() && (I.frames.front().pts <= clk + 0.005 || (!I.playing && !got))) {
        rgba.swap(I.frames.front().rgba);
        fw = I.frames.front().w;
        fh = I.frames.front().h;
        I.frames.pop_front();
        got = true;
    }
    return got;
}

}  // namespace sf

#else  // ---- no playback backend on this platform ----------------------------------------------------

namespace sf {
struct MediaPlayer::Impl {};
MediaPlayer::MediaPlayer() = default;
MediaPlayer::~MediaPlayer() = default;
MediaPlayer::MediaPlayer(MediaPlayer&&) noexcept = default;
MediaPlayer& MediaPlayer::operator=(MediaPlayer&&) noexcept = default;
bool MediaPlayer::spectrum(float*, int) { return false; }
bool MediaPlayer::open(const std::string&) { return false; }
void MediaPlayer::close() {}
bool MediaPlayer::isOpen() const { return false; }
bool MediaPlayer::hasVideo() const { return false; }
bool MediaPlayer::hasAudio() const { return false; }
int MediaPlayer::videoWidth() const { return 0; }
int MediaPlayer::videoHeight() const { return 0; }
double MediaPlayer::duration() const { return 0; }
void MediaPlayer::play() {}
void MediaPlayer::pause() {}
bool MediaPlayer::playing() const { return false; }
void MediaPlayer::seek(double) {}
double MediaPlayer::position() const { return 0; }
void MediaPlayer::setVolume(float) {}
float MediaPlayer::volume() const { return 1; }
bool MediaPlayer::ended() const { return false; }
bool MediaPlayer::takeFrame(std::vector<uint8_t>&) { return false; }
bool MediaPlayer::takeFrame(std::vector<uint8_t>&, int&, int&) { return false; }
void MediaPlayer::setMaxVideoSize(int, int) {}
}  // namespace sf

#endif
