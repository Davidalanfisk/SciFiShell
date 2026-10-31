#define NOMINMAX
#include "WinCapture.h"

#include <windows.h>

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <cstring>

namespace sf {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace dx = winrt::Windows::Graphics::DirectX;
namespace d3dw = winrt::Windows::Graphics::DirectX::Direct3D11;

struct WinCaptureImpl {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    winrt::com_ptr<ID3D11Texture2D> staging;
    d3dw::IDirect3DDevice winDevice{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 poolSize{0, 0};
    std::vector<uint32_t> last;
    int lw = 0, lh = 0;
    bool got = false, running = false, apartment = false;
};

WinCapture::WinCapture() : d_(new WinCaptureImpl) {}
WinCapture::~WinCapture() { stop(); }

bool WinCapture::active() const { return d_->running; }

bool WinCapture::start(HWND h) {
    stop();
    try {
        if (!wgc::GraphicsCaptureSession::IsSupported()) return false;
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        d_->apartment = true;

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL level{};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, d_->device.put(), &level,
                                     d_->ctx.put())))
            return false;
        // The capture runs on system threads while we map frames on ours: the device must serialise its own access.
        if (auto mt = d_->device.try_as<ID3D11Multithread>()) mt->SetMultithreadProtected(TRUE);
        auto dxgi = d_->device.as<IDXGIDevice>();
        winrt::com_ptr<::IInspectable> insp;
        if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put()))) return false;
        d_->winDevice = insp.as<d3dw::IDirect3DDevice>();

        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (FAILED(interop->CreateForWindow(h, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(d_->item)))) return false;

        d_->poolSize = d_->item.Size();
        if (d_->poolSize.Width <= 0 || d_->poolSize.Height <= 0) return false;
        d_->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(d_->winDevice, dx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
                                                                      d_->poolSize);
        d_->session = d_->pool.CreateCaptureSession(d_->item);
        try {
            d_->session.IsCursorCaptureEnabled(false);
        } catch (...) {
        }
        try {
            d_->session.IsBorderRequired(false);  // newer Windows only
        } catch (...) {
        }
        d_->session.StartCapture();
        d_->running = true;
        return true;
    } catch (...) {
        stop();
        return false;
    }
}

void WinCapture::stop() {
    try {
        if (d_->session) d_->session.Close();
        if (d_->pool) d_->pool.Close();
    } catch (...) {
    }
    d_->session = nullptr;
    d_->pool = nullptr;
    d_->item = nullptr;
    d_->winDevice = nullptr;
    d_->staging = nullptr;
    d_->ctx = nullptr;
    d_->device = nullptr;
    d_->last.clear();
    d_->lw = d_->lh = 0;
    d_->got = false;
    d_->running = false;
}

bool WinCapture::latest(std::vector<uint32_t>& bgra, int& w, int& h) {
    if (!d_->running) return false;
    try {
        wgc::Direct3D11CaptureFrame frame{nullptr};
        // Drain to the newest frame: only the latest matters.
        while (auto f = d_->pool.TryGetNextFrame()) frame = f;
        if (frame) {
            const auto cs = frame.ContentSize();
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> tex;
            access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), tex.put_void());
            D3D11_TEXTURE2D_DESC desc{};
            tex->GetDesc(&desc);
            if (!d_->staging || [&] {
                    D3D11_TEXTURE2D_DESC sd{};
                    d_->staging->GetDesc(&sd);
                    return sd.Width != desc.Width || sd.Height != desc.Height;
                }()) {
                D3D11_TEXTURE2D_DESC sd = desc;
                sd.Usage = D3D11_USAGE_STAGING;
                sd.BindFlags = 0;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                sd.MiscFlags = 0;
                d_->staging = nullptr;
                d_->device->CreateTexture2D(&sd, nullptr, d_->staging.put());
            }
            if (d_->staging) {
                d_->ctx->CopyResource(d_->staging.get(), tex.get());
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(d_->ctx->Map(d_->staging.get(), 0, D3D11_MAP_READ, 0, &m))) {
                    const int cw = std::min<int>(cs.Width, desc.Width), ch = std::min<int>(cs.Height, desc.Height);
                    d_->last.resize(static_cast<size_t>(cw) * ch);
                    for (int y = 0; y < ch; ++y)
                        std::memcpy(&d_->last[static_cast<size_t>(y) * cw], static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                                    static_cast<size_t>(cw) * 4);
                    d_->ctx->Unmap(d_->staging.get(), 0);
                    d_->lw = cw;
                    d_->lh = ch;
                    d_->got = true;
                }
            }
            // The window was resized: the pool must match or later frames are scaled.
            if (cs.Width != d_->poolSize.Width || cs.Height != d_->poolSize.Height) {
                d_->poolSize = cs;
                d_->pool.Recreate(d_->winDevice, dx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, cs);
            }
            frame.Close();
        }
    } catch (...) {
        return d_->got && (bgra = d_->last, w = d_->lw, h = d_->lh, true);
    }
    if (!d_->got) return false;
    bgra = d_->last;
    w = d_->lw;
    h = d_->lh;
    return true;
}

}  // namespace sf
