#include "MediaPlayer.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <SDL.h>
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
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
}  // namespace

struct MediaPlayer::Impl {
    struct Frame {
        double pts = 0;
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

    // Clock for files without audio.
    double clockBase = 0;
    Clock::time_point wallStart;

    // Worker-only.
    ComPtr<IMFSourceReader> reader;
    int vIdx = -1, aIdx = -1;
    LONG vStride = 0;
    bool vEos = false, aEos = false;

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

    bool openReader() {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (wlen <= 0) return false;
        std::wstring wpath(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);

        ComPtr<IMFAttributes> attrs;
        if (FAILED(MFCreateAttributes(&attrs, 2))) return false;
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
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
            want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
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
        ComPtr<IMFMediaBuffer> buffer;
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
            f.rgba.resize(static_cast<size_t>(w) * h * 4);
            for (int y = 0; y < h; ++y) {
                const BYTE* srow = data + (stride < 0 ? (chgt - 1 - y) : y) * absStride;
                uint8_t* drow = &f.rgba[static_cast<size_t>(y) * w * 4];
                for (int x = 0; x < w; ++x) {
                    drow[x * 4 + 0] = srow[x * 4 + 2];
                    drow[x * 4 + 1] = srow[x * 4 + 1];
                    drow[x * 4 + 2] = srow[x * 4 + 0];
                    drow[x * 4 + 3] = 255;
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
        bool ok = mf && openReader();
        {
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

bool MediaPlayer::takeFrame(std::vector<uint8_t>& rgba) {
    if (!impl_) return false;
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.m);
    bool got = false;
    double clk = I.clockLocked();
    while (!I.frames.empty() && (I.frames.front().pts <= clk + 0.005 || (!I.playing && !got))) {
        rgba.swap(I.frames.front().rgba);
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
}  // namespace sf

#endif
