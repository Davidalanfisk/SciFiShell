// Video frame extraction for thumbnails (Windows Media Foundation).
#include "Thumbnails.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

namespace sf {

namespace {
struct MfRuntime {
    bool ok;
    MfRuntime() : ok(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {}
    ~MfRuntime() { if (ok) MFShutdown(); }
};
}  // namespace

bool extractVideoFrame(const std::string& utf8Path, Bitmap& out) {
    static thread_local MfRuntime runtime;
    if (!runtime.ok) return false;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring wpath(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, wpath.data(), wlen);

    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 2))) return false;
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(wpath.c_str(), attrs.Get(), &reader))) return false;

    ComPtr<IMFMediaType> want;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want.Get()))) return false;
    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    PROPVARIANT dur;
    PropVariantInit(&dur);
    if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &dur)) &&
        dur.vt == VT_UI8 && dur.uhVal.QuadPart > 0) {
        const LONGLONG target = static_cast<LONGLONG>(dur.uhVal.QuadPart / 5);  // about 20% of the way in
        PROPVARIANT pos;
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = target;
        reader->SetCurrentPosition(GUID_NULL, pos);
    }
    PropVariantClear(&dur);

    for (int attempt = 0; attempt < 60; ++attempt) {
        DWORD streamIndex = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &ts, &sample))) return false;
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) return false;
        if (!sample) continue;

        ComPtr<IMFMediaType> type;
        if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type))) return false;
        UINT32 w = 0, h = 0;
        if (FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h)) || !w || !h) return false;
        UINT32 rawStride = 0;
        LONG stride;
        if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride))) stride = static_cast<LONG>(static_cast<INT32>(rawStride));
        else if (FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, w, &stride))) stride = static_cast<LONG>(w * 4);

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
        BYTE* data = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(buffer->Lock(&data, &maxLen, &curLen))) return false;
        size_t absStride = static_cast<size_t>(std::labs(stride));
        bool ok = absStride >= w * 4 && curLen >= absStride * h;
        if (ok) {
            out.w = static_cast<int>(w);
            out.h = static_cast<int>(h);
            out.rgba.resize(static_cast<size_t>(w) * h * 4);
            for (UINT32 y = 0; y < h; ++y) {
                const BYTE* srow = data + (stride < 0 ? (h - 1 - y) : y) * absStride;
                uint8_t* drow = &out.rgba[static_cast<size_t>(y) * w * 4];
                for (UINT32 x = 0; x < w; ++x) {
                    drow[x * 4 + 0] = srow[x * 4 + 2];
                    drow[x * 4 + 1] = srow[x * 4 + 1];
                    drow[x * 4 + 2] = srow[x * 4 + 0];
                    drow[x * 4 + 3] = 255;
                }
            }
        }
        buffer->Unlock();
        return ok;
    }
    return false;
}

}  // namespace sf

#else

namespace sf {
bool extractVideoFrame(const std::string&, Bitmap&) { return false; }
}  // namespace sf

#endif
